#include "rideflow/engine.hpp"
#include <iomanip>
#include <iostream>

class ConsoleObserver final : public rideflow::TripObserver {
public:
    void onTripUpdate(const rideflow::TripEvent& event) override {
        const auto& t = event.trip;
        std::cout << "Trip #" << t.id << " | " << rideflow::toString(t.status)
                  << " | driver " << t.driverId << " | INR "
                  << std::fixed << std::setprecision(2) << t.fare << '\n';
    }
};
int main() {
    try {
        rideflow::Engine engine;
        auto observer = std::make_shared<ConsoleObserver>();
        engine.subscribe(observer);
        engine.fleet().add("driver-001", {0,0});
        engine.fleet().add("driver-002", {5,5});
        const auto trip = engine.book("rider-001", {1,0}, {4,4});
        engine.advance(trip.id, rideflow::Status::Arriving);
        engine.advance(trip.id, rideflow::Status::InProgress);
        engine.advance(trip.id, rideflow::Status::Completed);
        engine.flushEvents();
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
