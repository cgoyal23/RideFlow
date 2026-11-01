CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -pthread
CPPFLAGS += -Iinclude
.PHONY: all test clean
all: build/rideflow_demo build/rideflow_tests
build:
	mkdir -p build
build/rideflow_demo: src/engine.cpp examples/demo.cpp include/rideflow/engine.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/engine.cpp examples/demo.cpp -o $@
build/rideflow_tests: src/engine.cpp tests/engine_tests.cpp include/rideflow/engine.hpp | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) src/engine.cpp tests/engine_tests.cpp -o $@
test: build/rideflow_tests
	./build/rideflow_tests
clean:
	rm -rf build
