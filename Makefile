CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Iinclude
BUILD_DIR ?= build

ENGINE_SRC = src/formatting.cpp src/order_book.cpp
DEMO_BIN = $(BUILD_DIR)/tradematch_demo
BENCH_BIN = $(BUILD_DIR)/tradematch_benchmark
TEST_BIN = $(BUILD_DIR)/tradematch_tests

.PHONY: all test demo benchmark cmake-configure cmake-build clean

all: $(DEMO_BIN) $(BENCH_BIN) $(TEST_BIN)

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(DEMO_BIN): $(ENGINE_SRC) tools/demo_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(BENCH_BIN): $(ENGINE_SRC) tools/benchmark_main.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

$(TEST_BIN): $(ENGINE_SRC) tests/order_book_tests.cpp | $(BUILD_DIR)
	$(CXX) $(CXXFLAGS) $^ -o $@

test: $(TEST_BIN)
	./$(TEST_BIN)

demo: $(DEMO_BIN)
	./$(DEMO_BIN)

benchmark: $(BENCH_BIN)
	./$(BENCH_BIN)

cmake-configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release

cmake-build: cmake-configure
	cmake --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)
