CXX ?= clang++
CXXFLAGS ?= -std=c++11 -Wall -Wextra -O2 -pthread
TARGETS = bin/service_demo bin/client_demo

all: $(TARGETS)

bin/service_demo: service_demo.cpp someip.hpp sd.hpp net.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ service_demo.cpp

bin/client_demo: client_demo.cpp someip.hpp sd.hpp net.hpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ client_demo.cpp

clean:
	rm -rf bin

.PHONY: all clean