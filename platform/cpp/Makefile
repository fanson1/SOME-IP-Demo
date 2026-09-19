.PHONY: all test clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude -pthread

EXAMPLES = bin/service_demo bin/client_demo
TESTS    = build/test_wire_ser build/test_transport build/test_tpc build/test_sdm build/test_config build/test_log build/test_app

all: $(EXAMPLES)

test: $(TESTS)
	./build/test_wire_ser
	./build/test_transport
	./build/test_tpc
	./build/test_sdm
	./build/test_config
	./build/test_log
	./build/test_app

bin/service_demo: examples/service_demo.cpp include/someip/app.hpp include/someip/sdm.hpp include/someip/transport.hpp include/someip/tpc.hpp include/someip/wire.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ examples/service_demo.cpp

bin/client_demo: examples/client_demo.cpp include/someip/app.hpp include/someip/sdm.hpp include/someip/transport.hpp include/someip/tpc.hpp include/someip/wire.hpp
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

build/test_config: tests/test_config.cpp include/someip/config.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_config.cpp

build/test_log: tests/test_log.cpp include/someip/log.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_log.cpp

build/test_app: tests/test_app.cpp include/someip/app.hpp include/someip/sdm.hpp include/someip/transport.hpp include/someip/tpc.hpp include/someip/wire.hpp include/someip/types.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ tests/test_app.cpp

clean:
	rm -rf bin build