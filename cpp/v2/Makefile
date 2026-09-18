.PHONY: all test clean

CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Iinclude

TESTS = build/test_wire_ser

all: $(TESTS)

build/test_wire_ser: tests/test_wire_ser.cpp include/someip2/wire.hpp include/someip2/ser.hpp include/someip2/types.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) tests/test_wire_ser.cpp -o $@

test: $(TESTS)
	./build/test_wire_ser

clean:
	rm -rf build