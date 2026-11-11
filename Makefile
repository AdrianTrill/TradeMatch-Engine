CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow
CPPFLAGS += -Iinclude
DEPFLAGS = -MMD -MP
BUILD_DIR ?= build
OBJ_DIR := $(BUILD_DIR)/obj

ENGINE_SRC := src/formatting.cpp src/order_book.cpp
ENGINE_OBJ := $(ENGINE_SRC:%.cpp=$(OBJ_DIR)/%.o)

DEMO_BIN := $(BUILD_DIR)/tradematch_demo
BENCH_BIN := $(BUILD_DIR)/tradematch_benchmark
TEST_BIN := $(BUILD_DIR)/tradematch_tests

ALL_OBJ := $(ENGINE_OBJ) \
	$(OBJ_DIR)/tools/demo_main.o \
	$(OBJ_DIR)/tools/benchmark_main.o \
	$(OBJ_DIR)/tests/order_book_tests.o

.PHONY: all test demo benchmark cmake-configure cmake-build clean

all: $(DEMO_BIN) $(BENCH_BIN) $(TEST_BIN)

# Each object also emits a .d file listing the headers it includes, so header edits trigger rebuilds.
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(DEMO_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/demo_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BENCH_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/benchmark_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/order_book_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

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

-include $(ALL_OBJ:.o=.d)
