# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 The Pantheon authors

# Convenience wrapper for the dependency-free DSP core + tests (no CMake/JUCE needed).
CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wconversion -Wno-sign-conversion -Icore/include -I../../shared/dsp/include

build/themis_tests: tests/test_main.cpp $(wildcard core/include/themis/*.h)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) tests/test_main.cpp -o $@

.PHONY: test clean
test: build/themis_tests
	./build/themis_tests

clean:
	rm -rf build
