CXX ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -pthread
BUILD := build
COMMON := protocol.cpp metrics.cpp
HEADERS := json.hpp protocol.hpp metrics.hpp

all: $(BUILD)/agent $(BUILD)/server

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/agent: agent.cpp $(COMMON) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ agent.cpp $(COMMON)

$(BUILD)/server: server.cpp protocol.cpp json.hpp protocol.hpp | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ server.cpp protocol.cpp

$(BUILD)/tests: tests.cpp $(COMMON) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -o $@ tests.cpp $(COMMON)

test: $(BUILD)/tests
	./$(BUILD)/tests

# Cross-language test: C++ agent <-> Python server, Python client <-> C++ server
cross: all
	python3 cross_test.py

clean:
	rm -rf $(BUILD)

.PHONY: all test cross clean
