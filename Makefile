CXX ?= clang++
LLVM_CONFIG ?= llvm-config
CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Wpedantic $(shell $(LLVM_CONFIG) --cxxflags)
LDFLAGS := $(shell $(LLVM_CONFIG) --ldflags --system-libs --libs core support)
RUNTIME_SOURCE := $(CURDIR)/src/runtime.cpp
SANDBOX_RUNNER := $(CURDIR)/runtime/sandbox_runner.py

.PHONY: all check clean
all: pact

pact: src/pact.cpp src/runtime.cpp $(SANDBOX_RUNNER)
	$(CXX) $(CXXFLAGS) -DPACT_RUNTIME_SOURCE='"$(RUNTIME_SOURCE)"' -DPACT_SANDBOX_RUNNER='"$(SANDBOX_RUNNER)"' $< $(LDFLAGS) -o $@

check: pact
	./tests/run.sh

clean:
	rm -f pact build/*
