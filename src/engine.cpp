#include "rideflow/engine.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace rideflow {
namespace {
void validate(Location p) {
    if (!std::isfinite(p.x) || !std::isfinite(p.y))
        throw std::invalid_argument("Coordinates must be finite");
}
template<Status Current> class State final : public TripState {
public:
    bool allows(Status next) const override {
        switch (Current) {
        case Status::Requested: return next == Status::Assigned || next == Status::Cancelled;
        case Status::Assigned: return next == Status::Arriving || next == Status::Cancelled;
        case Status::Arriving: return next == Status::InProgress || next == Status::Cancelled;
        case Status::InProgress: return next == Status::Completed;
        default: return false;
        }
    }
};
}
double distance(Location a, Location b) { return std::hypot(a.x-b.x, a.y-b.y); }
std::string toString(Status s) {
    switch (s) {
    case Status::Requested: return "requested";
    case Status::Assigned: return "assigned";
    case Status::Arriving: return "arriving";
    case Status::InProgress: return "in_progress";
    case Status::Completed: return "completed";
    case Status::Cancelled: return "cancelled";
    }
    throw std::invalid_argument("Unknown status");
}
const TripState& TripState::forStatus(Status s) {
    static const State<Status::Requested> requested;
    static const State<Status::Assigned> assigned;
    static const State<Status::Arriving> arriving;
    static const State<Status::InProgress> inProgress;
    static const State<Status::Completed> completed;
    static const State<Status::Cancelled> cancelled;
    switch (s) {
    case Status::Requested: return requested;
    case Status::Assigned: return assigned;
    case Status::Arriving: return arriving;
    case Status::InProgress: return inProgress;
    case Status::Completed: return completed;
    case Status::Cancelled: return cancelled;
    }
    throw std::invalid_argument("Unknown status");
}
std::string NearestDriver::select(const std::vector<Driver>& candidates, Location pickup) const {
    if (candidates.empty()) return {};
    const auto best = std::min_element(candidates.begin(), candidates.end(), [&](const Driver& a, const Driver& b) {
        const auto da = distance(a.location, pickup), db = distance(b.location, pickup);
        return da == db ? a.id < b.id : da < db;
    });
    return best->id;
}
SurgePricing::SurgePricing(double base, double perKm, double cap)
    : baseFare_(base), perKm_(perKm), maxMultiplier_(cap) {
    if (!std::isfinite(base) || !std::isfinite(perKm) || !std::isfinite(cap) || base < 0 || perKm < 0 || cap < 1)
        throw std::invalid_argument("Invalid pricing parameters");
}
double SurgePricing::quote(double km, std::size_t demand, std::size_t supply) const {
    if (!std::isfinite(km) || km < 0) throw std::invalid_argument("Invalid trip distance");
    const double ratio = static_cast<double>(demand) / static_cast<double>(std::max<std::size_t>(1, supply));
    const double fare = (baseFare_ + perKm_ * km) * std::clamp(ratio, 1.0, maxMultiplier_);
    if (!std::isfinite(fare) || !std::isfinite(fare * 100)) throw std::invalid_argument("Fare overflow");
    return std::round(fare * 100) / 100;
}
void Fleet::add(std::string id, Location p) {
    validate(p);
    if (id.empty()) throw std::invalid_argument("Driver ID is required");
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (drivers_.count(id)) throw std::invalid_argument("Duplicate driver ID");
    drivers_.emplace(id, Driver{id, p, true});
}
void Fleet::updateLocation(const std::string& id, Location p) {
    validate(p);
    std::unique_lock<std::shared_mutex> lock(mutex_);
    drivers_.at(id).location = p;
}
std::vector<Driver> Fleet::available() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<Driver> result;
    for (const auto& entry : drivers_) if (entry.second.available) result.push_back(entry.second);
    return result;
}
Driver Fleet::get(const std::string& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return drivers_.at(id);
}
bool Fleet::reserve(const std::string& id) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = drivers_.find(id);
    if (it == drivers_.end() || !it->second.available) return false;
    it->second.available = false;
    return true;
}
void Fleet::release(const std::string& id, Location p) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    auto& driver = drivers_.at(id);
    driver.location = p;
    driver.available = true;
}
EventDispatcher::EventDispatcher() : worker_([this] { run(); }) {}
EventDispatcher::~EventDispatcher() {
    { std::lock_guard<std::mutex> lock(mutex_); stopping_ = true; }
    ready_.notify_one();
    worker_.join();
}
void EventDispatcher::subscribe(const std::shared_ptr<TripObserver>& observer) {
    if (!observer) throw std::invalid_argument("Observer is required");
    std::lock_guard<std::mutex> lock(mutex_);
    observers_.push_back(observer);
}
void EventDispatcher::publish(TripEvent event) {
    { std::lock_guard<std::mutex> lock(mutex_); queue_.push_back(std::move(event)); }
    ready_.notify_one();
}
void EventDispatcher::flush() {
    if (std::this_thread::get_id() == worker_.get_id())
        throw std::logic_error("Cannot flush events from an observer callback");
    std::unique_lock<std::mutex> lock(mutex_);
    idle_.wait(lock, [this] { return queue_.empty() && !delivering_; });
}
std::size_t EventDispatcher::failures() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return failures_;
}
void EventDispatcher::run() {
    for (;;) {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_.wait(lock, [this] { return stopping_ || !queue_.empty(); });
        if (queue_.empty() && stopping_) break;
        auto event = std::move(queue_.front()); queue_.pop_front();
        std::vector<std::shared_ptr<TripObserver>> listeners;
        observers_.erase(std::remove_if(observers_.begin(), observers_.end(), [&](auto& weak) {
            auto observer = weak.lock();
            if (!observer) return true;
            listeners.push_back(std::move(observer)); return false;
        }), observers_.end());
        delivering_ = true;
        lock.unlock();
        for (const auto& listener : listeners) {
            try { listener->onTripUpdate(event); }
            catch (...) { std::lock_guard<std::mutex> guard(mutex_); ++failures_; }
        }
        lock.lock(); delivering_ = false;
        if (queue_.empty()) idle_.notify_all();
    }
}
Engine::Engine(std::shared_ptr<const DispatchStrategy> dispatch, std::shared_ptr<const PricingStrategy> pricing)
    : dispatch_(std::move(dispatch)), pricing_(std::move(pricing)) {
    if (!dispatch_ || !pricing_) throw std::invalid_argument("Strategies are required");
}
Trip Engine::book(std::string riderId, Location pickup, Location destination) {
    validate(pickup); validate(destination);
    if (riderId.empty()) throw std::invalid_argument("Rider ID is required");
    std::lock_guard<std::mutex> lock(mutex_);
    const auto candidates = fleet_.available();
    if (candidates.empty()) throw std::runtime_error("No drivers available");
    const auto driverId = dispatch_->select(candidates, pickup);
    if (std::none_of(candidates.begin(), candidates.end(), [&](const Driver& d) { return d.id == driverId; }))
        throw std::logic_error("Dispatch strategy selected an invalid driver");
    std::size_t demand = 1;
    for (const auto& entry : trips_)
        if (entry.second.status != Status::Completed && entry.second.status != Status::Cancelled) ++demand;
    const auto fare = pricing_->quote(distance(pickup, destination), demand, candidates.size());
    if (!std::isfinite(fare) || fare < 0) throw std::logic_error("Pricing strategy returned an invalid fare");
    Trip trip{nextId_++, std::move(riderId), driverId, pickup, destination, fare, Status::Assigned};
    // Insertion can allocate; do it before reserving a driver.
    trips_.emplace(trip.id, trip);
    if (!fleet_.reserve(driverId)) { trips_.erase(trip.id); throw std::runtime_error("Driver unavailable"); }
    auto requested = trip; requested.status = Status::Requested;
    events_.publish({requested}); events_.publish({trip});
    return trip;
}
Trip Engine::advance(std::uint64_t id, Status next) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto& trip = trips_.at(id);
    if (!TripState::forStatus(trip.status).allows(next)) throw std::logic_error("Invalid trip transition");
    trip.status = next;
    if (next == Status::Completed) fleet_.release(trip.driverId, trip.destination);
    if (next == Status::Cancelled) fleet_.release(trip.driverId, fleet_.get(trip.driverId).location);
    events_.publish({trip});
    return trip;
}
Trip Engine::get(std::uint64_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return trips_.at(id);
}
} // namespace rideflow
