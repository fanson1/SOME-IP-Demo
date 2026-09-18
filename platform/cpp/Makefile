.PHONY: all test clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -pthread

EXAMPLES = bin/service_demo bin/client_demo
TESTS    = build/test_wire_ser

all: $(EXAMPLES)

test: $(TESTS)
	./build/test_wire_ser

bin/service_demo: examples/service_demo.cpp include/someip/legacy/someip.hpp include/someip/legacy/sd.hpp include/someip/legacy/net.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ examples/service_demo.cpp

bin/client_demo: examples/client_demo.cpp include/someip/legacy/someip.hpp include/someip/legacy/sd.hpp include/someip/legacy/net.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ examples/client_demo.cpp

build/test_wire_ser: tests/test_wire_ser.cpp include/someip/wire.hpp include/someip/ser.hpp include/someip/types.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_wire_ser.cpp

clean:
	rm -rf bin build