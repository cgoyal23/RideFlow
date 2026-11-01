#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace rideflow {

// Cartesian coordinates in kilometres; suitable for a local dispatch simulation.
struct Location { double x = 0; double y = 0; };
double distance(Location a, Location b);
struct Driver { std::string id; Location location; bool available = true; };
enum class Status { Requested, Assigned, Arriving, InProgress, Completed, Cancelled };
std::string toString(Status status);
struct Trip {
    std::uint64_t id;
    std::string riderId;
    std::string driverId;
    Location pickup;
    Location destination;
    double fare;
    Status status;
};

// Strategies receive value snapshots, so implementations never hold fleet locks.
class DispatchStrategy {
public:
    virtual ~DispatchStrategy() = default;
    virtual std::string select(const std::vector<Driver>& candidates, Location pickup) const = 0;
};
class NearestDriver final : public DispatchStrategy {
public:
    std::string select(const std::vector<Driver>& candidates, Location pickup) const override;
};
class PricingStrategy {
public:
    virtual ~PricingStrategy() = default;
    virtual double quote(double kilometres, std::size_t demand, std::size_t supply) const = 0;
};
class SurgePricing final : public PricingStrategy {
public:
    SurgePricing(double baseFare = 40, double perKm = 12, double maxMultiplier = 3);
    double quote(double kilometres, std::size_t demand, std::size_t supply) const override;
private:
    double baseFare_, perKm_, maxMultiplier_;
};

class Fleet {
public:
    void add(std::string id, Location location);
    void updateLocation(const std::string& id, Location location);
    std::vector<Driver> available() const;
    Driver get(const std::string& id) const;
private:
    friend class Engine;
    bool reserve(const std::string& id);
    void release(const std::string& id, Location location);
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, Driver> drivers_;
};

// State objects enforce allowed lifecycle transitions.
class TripState {
public:
    virtual ~TripState() = default;
    virtual bool allows(Status next) const = 0;
    static const TripState& forStatus(Status status);
};

struct TripEvent { Trip trip; };
class TripObserver {
public:
    virtual ~TripObserver() = default;
    virtual void onTripUpdate(const TripEvent& event) = 0;
};

// FIFO worker, weak subscriptions, and a draining RAII shutdown.
class EventDispatcher {
public:
    EventDispatcher();
    ~EventDispatcher();
    EventDispatcher(const EventDispatcher&) = delete;
    EventDispatcher& operator=(const EventDispatcher&) = delete;
    void subscribe(const std::shared_ptr<TripObserver>& observer);
    void publish(TripEvent event);
    void flush();
    std::size_t failures() const;
private:
    void run();
    mutable std::mutex mutex_;
    std::condition_variable ready_, idle_;
    std::deque<TripEvent> queue_;
    std::vector<std::weak_ptr<TripObserver>> observers_;
    bool stopping_ = false;
    bool delivering_ = false;
    std::size_t failures_ = 0;
    std::thread worker_;
};

class Engine {
public:
    Engine(std::shared_ptr<const DispatchStrategy> dispatch = std::make_shared<NearestDriver>(),
           std::shared_ptr<const PricingStrategy> pricing = std::make_shared<SurgePricing>());
    Fleet& fleet() { return fleet_; }
    void subscribe(const std::shared_ptr<TripObserver>& observer) { events_.subscribe(observer); }
    Trip book(std::string riderId, Location pickup, Location destination);
    Trip advance(std::uint64_t id, Status next);
    Trip get(std::uint64_t id) const;
    void flushEvents() { events_.flush(); }
    std::size_t observerFailures() const { return events_.failures(); }
private:
    Fleet fleet_;
    std::shared_ptr<const DispatchStrategy> dispatch_;
    std::shared_ptr<const PricingStrategy> pricing_;
    mutable std::mutex mutex_;
    std::unordered_map<std::uint64_t, Trip> trips_;
    std::uint64_t nextId_ = 1;
    EventDispatcher events_;
};
} // namespace rideflow
