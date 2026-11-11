CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wshadow
CPPFLAGS += -Iinclude -Itests -Ibench
DEPFLAGS = -MMD -MP
BUILD_DIR ?= build
OBJ_DIR := $(BUILD_DIR)/obj

ENGINE_SRC := src/formatting.cpp src/order_book.cpp
ENGINE_OBJ := $(ENGINE_SRC:%.cpp=$(OBJ_DIR)/%.o)

DEMO_BIN := $(BUILD_DIR)/tradematch_demo
BENCH_BIN := $(BUILD_DIR)/tradematch_benchmark
TEST_BIN := $(BUILD_DIR)/tradematch_tests
DIFF_TEST_BIN := $(BUILD_DIR)/tradematch_differential_tests
FUZZ_BIN := $(BUILD_DIR)/tradematch_fuzz_standalone
BENCH_TEST_BIN := $(BUILD_DIR)/tradematch_bench_tests
INDEX_TEST_BIN := $(BUILD_DIR)/tradematch_order_index_tests
ALLOC_TEST_BIN := $(BUILD_DIR)/tradematch_allocation_tests

SOURCES = $(shell git ls-files '*.cpp' '*.hpp')
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
GCOVR ?= gcovr

ifeq ($(shell uname -s),Darwin)
# Apple keeps libc++ inside the SDK, so standalone LLVM tools need the sysroot; gcov data is read by llvm-cov.
TIDY_FLAGS += --extra-arg=-isysroot --extra-arg=$(shell xcrun --show-sdk-path)
GCOV ?= xcrun llvm-cov gcov
else
GCOV ?= gcov
endif

ALL_OBJ := $(ENGINE_OBJ) \
	$(OBJ_DIR)/tools/demo_main.o \
	$(OBJ_DIR)/bench/benchmark_main.o \
	$(OBJ_DIR)/bench/workload.o \
	$(OBJ_DIR)/bench/baseline/map_order_book.o \
	$(OBJ_DIR)/tests/order_book_tests.o \
	$(OBJ_DIR)/tests/differential_tests.o \
	$(OBJ_DIR)/tests/bench_tests.o \
	$(OBJ_DIR)/tests/order_index_tests.o \
	$(OBJ_DIR)/tests/allocation_tests.o \
	$(OBJ_DIR)/fuzz/order_book_fuzzer.o \
	$(OBJ_DIR)/fuzz/standalone_main.o

.PHONY: all test demo benchmark fuzz-long format format-check lint sanitize coverage cmake-configure \
	cmake-build clean

all: $(DEMO_BIN) $(BENCH_BIN) $(TEST_BIN) $(DIFF_TEST_BIN) $(FUZZ_BIN) $(BENCH_TEST_BIN) $(INDEX_TEST_BIN) \
	$(ALLOC_TEST_BIN)

# Each object also emits a .d file listing the headers it includes, so header edits trigger rebuilds.
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(DEMO_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/demo_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BENCH_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/bench/benchmark_main.o $(OBJ_DIR)/bench/workload.o \
		$(OBJ_DIR)/bench/baseline/map_order_book.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/order_book_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(DIFF_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/differential_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(INDEX_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/order_index_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(ALLOC_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/allocation_tests.o $(OBJ_DIR)/bench/workload.o \
		$(OBJ_DIR)/bench/baseline/map_order_book.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BENCH_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/bench_tests.o $(OBJ_DIR)/bench/workload.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(FUZZ_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/fuzz/order_book_fuzzer.o $(OBJ_DIR)/fuzz/standalone_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_BIN) $(DIFF_TEST_BIN) $(FUZZ_BIN) $(BENCH_TEST_BIN) $(INDEX_TEST_BIN) $(ALLOC_TEST_BIN)
	./$(TEST_BIN)
	./$(INDEX_TEST_BIN)
	./$(ALLOC_TEST_BIN)
	./$(BENCH_TEST_BIN)
	./$(DIFF_TEST_BIN)
	./$(FUZZ_BIN) --iterations 500

# Longer randomized soak: 2,000 seeds x 5,000 operations against the reference book.
fuzz-long: $(DIFF_TEST_BIN) $(FUZZ_BIN)
	./$(DIFF_TEST_BIN) --seeds 2000 --ops 5000
	./$(FUZZ_BIN) --iterations 20000

demo: $(DEMO_BIN)
	./$(DEMO_BIN)

benchmark: $(BENCH_BIN)
	./$(BENCH_BIN)

format:
	$(CLANG_FORMAT) -i $(SOURCES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)

lint:
	cmake -S . -B $(BUILD_DIR)/lint -DCMAKE_BUILD_TYPE=Debug > /dev/null
	git ls-files '*.cpp' | xargs -P $(JOBS) -n 1 $(CLANG_TIDY) -p $(BUILD_DIR)/lint --quiet $(TIDY_FLAGS)

sanitize:
	cmake -S . -B $(BUILD_DIR)/sanitize -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTRADEMATCH_WARNINGS_AS_ERRORS=ON \
		-DTRADEMATCH_SANITIZERS=address,undefined > /dev/null
	cmake --build $(BUILD_DIR)/sanitize --parallel $(JOBS)
	ctest --test-dir $(BUILD_DIR)/sanitize --output-on-failure

coverage:
	cmake -S . -B $(BUILD_DIR)/coverage -DCMAKE_BUILD_TYPE=Debug -DTRADEMATCH_COVERAGE=ON > /dev/null
	cmake --build $(BUILD_DIR)/coverage --parallel $(JOBS)
	find $(BUILD_DIR)/coverage -name '*.gcda' -delete
	ctest --test-dir $(BUILD_DIR)/coverage --output-on-failure
	mkdir -p $(BUILD_DIR)/coverage/report
	$(GCOVR) --root . --object-directory $(BUILD_DIR)/coverage --gcov-executable "$(GCOV)" \
		--gcov-exclude-directories '.*tradematch_benchmark\.dir.*' \
		--filter 'src/' --filter 'include/' --exclude-unreachable-branches --exclude-throw-branches \
		--html-details $(BUILD_DIR)/coverage/report/index.html --print-summary
	@echo "HTML report: $(BUILD_DIR)/coverage/report/index.html"

cmake-configure:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Release

cmake-build: cmake-configure
	cmake --build $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR)

-include $(ALL_OBJ:.o=.d)
