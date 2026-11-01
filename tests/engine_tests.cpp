#include "rideflow/engine.hpp"
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

using namespace rideflow;
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
template<class E, class F> void throws(F action) {
    try { action(); } catch (const E&) { return; }
    throw std::runtime_error("Expected exception was not thrown");
}
struct Recorder final : TripObserver {
    std::vector<Status> statuses;
    std::vector<std::thread::id> threads;
    void onTripUpdate(const TripEvent& e) override {
        statuses.push_back(e.trip.status); threads.push_back(std::this_thread::get_id());
    }
};
struct BrokenObserver final : TripObserver {
    void onTripUpdate(const TripEvent&) override { throw std::runtime_error("Subscriber failed"); }
};
struct LastDriver final : DispatchStrategy {
    std::string select(const std::vector<Driver>& drivers, Location) const override {
        std::string last;
        for (auto& d : drivers) if (d.id > last) last = d.id;
        return last;
    }
};
struct FlatFare final : PricingStrategy {
    double quote(double, std::size_t, std::size_t) const override { return 99; }
};
struct InvalidDriver final : DispatchStrategy {
    std::string select(const std::vector<Driver>&, Location) const override { return "missing"; }
};
void lifecycle() {
    Engine engine;
    engine.fleet().add("b", {1,0}); engine.fleet().add("a", {-1,0});
    auto recorder = std::make_shared<Recorder>();
    auto broken = std::make_shared<BrokenObserver>();
    engine.subscribe(broken); engine.subscribe(recorder);
    auto trip = engine.book("rider", {0,0}, {3,4});
    check(trip.driverId == "a", "Nearest tie should use driver ID");
    check(trip.fare == 100, "Base fare should be 100");
    check(!engine.fleet().get("a").available, "Assigned driver must be busy");
    throws<std::logic_error>([&] { engine.advance(trip.id, Status::Completed); });
    throws<std::out_of_range>([&] { engine.get(999); });
    engine.advance(trip.id, Status::Arriving);
    engine.advance(trip.id, Status::InProgress);
    throws<std::logic_error>([&] { engine.advance(trip.id, Status::Cancelled); });
    engine.advance(trip.id, Status::Completed);
    throws<std::logic_error>([&] { engine.advance(trip.id, Status::Completed); });
    auto driver = engine.fleet().get("a");
    check(driver.available && driver.location.x == 3 && driver.location.y == 4, "Completion must release and relocate");
    engine.flushEvents();
    check(recorder->statuses == std::vector<Status>{Status::Requested, Status::Assigned, Status::Arriving, Status::InProgress, Status::Completed}, "Ordered lifecycle events");
    for (auto id : recorder->threads) check(id != std::this_thread::get_id(), "Callbacks must be asynchronous");
    check(engine.observerFailures() == 5, "Observer failures must be isolated and counted");
}
void cancellationAndValidation() {
    Engine e;
    e.fleet().add("one", {2,3});
    throws<std::invalid_argument>([&] { e.fleet().add("one", {}); });
    throws<std::invalid_argument>([&] { e.book("", {}, {}); });
    throws<std::invalid_argument>([&] { e.book("r", {std::numeric_limits<double>::infinity(),0}, {}); });
    throws<std::invalid_argument>([&] { e.subscribe(nullptr); });
    auto t = e.book("r", {}, {5,5});
    throws<std::runtime_error>([&] { e.book("r2", {}, {}); });
    e.advance(t.id, Status::Cancelled);
    check(e.fleet().get("one").available, "Cancellation must release driver");
    check(e.fleet().get("one").location.x == 2, "Cancellation must retain current location");
    auto t2 = e.book("r2", {}, {});
    e.advance(t2.id, Status::Arriving);
    e.advance(t2.id, Status::Cancelled);
    throws<std::logic_error>([&] { e.advance(t2.id, Status::Arriving); });
    check(e.fleet().available().size() == 1, "Arriving cancellation releases driver");
    SurgePricing p;
    check(p.quote(5, 1, 5) == 100, "Low demand baseline");
    check(p.quote(5, 4, 2) == 200, "Demand surge");
    check(p.quote(5, 100, 1) == 300, "Surge cap");
    throws<std::invalid_argument>([] { SurgePricing p(-1); });
    throws<std::invalid_argument>([&] { p.quote(-1, 1, 1); });
}
void strategies() {
    Engine e(std::make_shared<LastDriver>(), std::make_shared<FlatFare>());
    e.fleet().add("a", {}); e.fleet().add("z", {100,100});
    const auto trip = e.book("r", {}, {1,1});
    check(trip.driverId == "z" && trip.fare == 99, "Custom strategies must be used");
    Engine invalid(std::make_shared<InvalidDriver>());
    invalid.fleet().add("a", {});
    throws<std::logic_error>([&] { invalid.book("r", {}, {}); });
    check(invalid.fleet().available().size() == 1, "Bad strategy must not reserve driver");
}
void concurrentBookings() {
    Engine e;
    constexpr int driverCount = 24, requests = 96;
    for (int i=0; i<driverCount; ++i) e.fleet().add(std::to_string(i), {double(i),0});
    std::atomic<int> ready{0}, successes{0}, rejected{0};
    std::atomic<bool> start{false};
    std::mutex mutex;
    std::set<std::string> matched;
    std::set<std::uint64_t> ids;
    std::vector<std::thread> workers;
    for (int i=0; i<requests; ++i) workers.emplace_back([&,i] {
        ++ready;
        while (!start.load()) std::this_thread::yield();
        try {
            auto t = e.book("r"+std::to_string(i), {}, {1,1});
            std::lock_guard<std::mutex> lock(mutex);
            matched.insert(t.driverId); ids.insert(t.id); ++successes;
        } catch (const std::runtime_error&) { ++rejected; }
    });
    while (ready.load() < requests) std::this_thread::yield();
    start = true;
    for (auto& worker : workers) worker.join();
    check(successes == driverCount && rejected == requests-driverCount, "Capacity must be enforced concurrently");
    check(matched.size() == driverCount && ids.size() == driverCount, "No driver or booking ID may be duplicated");
    check(e.fleet().available().empty(), "All matched drivers must be busy");
    for (auto id : ids) e.advance(id, Status::Cancelled);
    check(e.fleet().available().size() == driverCount, "All cancelled drivers must be available");
}
void shutdownDrains() {
    auto recorder = std::make_shared<Recorder>();
    {
        Engine e;
        e.subscribe(recorder); e.fleet().add("a", {});
        auto t = e.book("r", {}, {}); e.advance(t.id, Status::Cancelled);
    }
    check(recorder->statuses.size() == 3, "Destructor must drain queued notifications");
}
int main() {
    try {
        lifecycle(); cancellationAndValidation(); strategies(); concurrentBookings(); shutdownDrains();
        std::cout << "PASS: lifecycle, validation, strategies, concurrent matching, async delivery, shutdown\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << '\n'; return 1; }
}
