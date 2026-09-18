.PHONY: all test clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -pthread

EXAMPLES = bin/service_demo bin/client_demo
TESTS    = build/test_wire_ser build/test_transport build/test_tpc build/test_sdm

all: $(EXAMPLES)

test: $(TESTS)
	./build/test_wire_ser
	./build/test_transport
	./build/test_tpc
	./build/test_sdm

bin/service_demo: examples/service_demo.cpp include/someip/legacy/someip.hpp include/someip/legacy/sd.hpp include/someip/legacy/net.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ examples/service_demo.cpp

bin/client_demo: examples/client_demo.cpp include/someip/legacy/someip.hpp include/someip/legacy/sd.hpp include/someip/legacy/net.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ examples/client_demo.cpp

build/test_wire_ser: tests/test_wire_ser.cpp include/someip/wire.hpp include/someip/ser.hpp include/someip/types.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_wire_ser.cpp

build/test_transport: tests/test_transport.cpp include/someip/transport.hpp include/someip/wire.hpp include/someip/types.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_transport.cpp

build/test_tpc: tests/test_tpc.cpp include/someip/tpc.hpp include/someip/wire.hpp include/someip/types.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_tpc.cpp

build/test_sdm: tests/test_sdm.cpp include/someip/sdm.hpp include/someip/wire.hpp include/someip/types.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_sdm.cpp

clean:
	rm -rf bin build