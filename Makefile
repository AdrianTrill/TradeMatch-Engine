CXX ?= c++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra -Wpedantic -Wshadow
CPPFLAGS += -Iinclude -Itests -Ibench
LDFLAGS += -pthread
DEPFLAGS = -MMD -MP
BUILD_DIR ?= build
OBJ_DIR := $(BUILD_DIR)/obj

ENGINE_SRC := src/cli.cpp src/codec.cpp src/file_io.cpp src/fix.cpp src/fix_gateway.cpp src/formatting.cpp \
	src/itch.cpp src/itch_feed.cpp src/journal.cpp src/market_data.cpp src/matching_engine.cpp src/order_book.cpp src/pipeline.cpp src/snapshot.cpp \
	src/socket.cpp
ENGINE_OBJ := $(ENGINE_SRC:%.cpp=$(OBJ_DIR)/%.o)

DEMO_BIN := $(BUILD_DIR)/tradematch_demo
TUI_BIN := $(BUILD_DIR)/tradematch_tui
REPLAY_BIN := $(BUILD_DIR)/tradematch_replay
CLI_BIN := $(BUILD_DIR)/tradematch_cli
CLI_TEST_BIN := $(BUILD_DIR)/tradematch_cli_tests
FIX_GATEWAY_BIN := $(BUILD_DIR)/tradematch_fix_gateway
FIX_CLIENT_BIN := $(BUILD_DIR)/tradematch_fix_client
FIX_TEST_BIN := $(BUILD_DIR)/tradematch_fix_tests
FIX_FUZZ_BIN := $(BUILD_DIR)/tradematch_fix_fuzz_standalone
ITCH_BIN := $(BUILD_DIR)/tradematch_itch
ITCH_TEST_BIN := $(BUILD_DIR)/tradematch_itch_tests
ITCH_FUZZ_BIN := $(BUILD_DIR)/tradematch_itch_fuzz_standalone
SCENARIOS := $(wildcard tests/scenarios/*.tm)
SIM_TEST_BIN := $(BUILD_DIR)/tradematch_simulator_tests
JOURNAL_TEST_BIN := $(BUILD_DIR)/tradematch_journal_tests
JOURNAL_FUZZ_BIN := $(BUILD_DIR)/tradematch_journal_fuzz_standalone
PIPELINE_TEST_BIN := $(BUILD_DIR)/tradematch_pipeline_tests
PIPELINE_BENCH_BIN := $(BUILD_DIR)/tradematch_pipeline_benchmark
BENCH_BIN := $(BUILD_DIR)/tradematch_benchmark
TEST_BIN := $(BUILD_DIR)/tradematch_tests
DIFF_TEST_BIN := $(BUILD_DIR)/tradematch_differential_tests
FUZZ_BIN := $(BUILD_DIR)/tradematch_fuzz_standalone
BENCH_TEST_BIN := $(BUILD_DIR)/tradematch_bench_tests
INDEX_TEST_BIN := $(BUILD_DIR)/tradematch_order_index_tests
MARKET_DATA_TEST_BIN := $(BUILD_DIR)/tradematch_market_data_tests
ALLOC_TEST_BIN := $(BUILD_DIR)/tradematch_allocation_tests

SOURCES = $(shell git ls-files '*.cpp' '*.hpp')
JOBS ?= $(shell getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
CLANG_FORMAT ?= clang-format
CLANG_TIDY ?= clang-tidy
GCOVR ?= gcovr
PYTHON ?= python3

ifeq ($(shell uname -s),Darwin)
# Apple keeps libc++ inside the SDK, so standalone LLVM tools need the sysroot; gcov data is read by llvm-cov.
TIDY_FLAGS += --extra-arg=-isysroot --extra-arg=$(shell xcrun --show-sdk-path)
GCOV ?= xcrun llvm-cov gcov
else
GCOV ?= gcov
endif

ALL_OBJ := $(ENGINE_OBJ) \
	$(OBJ_DIR)/tools/demo_main.o \
	$(OBJ_DIR)/tools/tui_main.o \
	$(OBJ_DIR)/tools/replay_main.o \
	$(OBJ_DIR)/tools/cli_main.o \
	$(OBJ_DIR)/tests/cli_tests.o \
	$(OBJ_DIR)/tools/fix_gateway_main.o \
	$(OBJ_DIR)/tools/fix_client_main.o \
	$(OBJ_DIR)/tests/fix_tests.o \
	$(OBJ_DIR)/fuzz/fix_fuzzer.o \
	$(OBJ_DIR)/tools/itch_main.o \
	$(OBJ_DIR)/tests/itch_tests.o \
	$(OBJ_DIR)/fuzz/itch_fuzzer.o \
	$(OBJ_DIR)/src/simulator.o \
	$(OBJ_DIR)/tests/simulator_tests.o \
	$(OBJ_DIR)/tests/journal_tests.o \
	$(OBJ_DIR)/tests/pipeline_tests.o \
	$(OBJ_DIR)/bench/pipeline_main.o \
	$(OBJ_DIR)/bench/benchmark_main.o \
	$(OBJ_DIR)/bench/workload.o \
	$(OBJ_DIR)/bench/baseline/map_order_book.o \
	$(OBJ_DIR)/tests/order_book_tests.o \
	$(OBJ_DIR)/tests/differential_tests.o \
	$(OBJ_DIR)/tests/bench_tests.o \
	$(OBJ_DIR)/tests/order_index_tests.o \
	$(OBJ_DIR)/tests/market_data_tests.o \
	$(OBJ_DIR)/tests/allocation_tests.o \
	$(OBJ_DIR)/fuzz/order_book_fuzzer.o \
	$(OBJ_DIR)/fuzz/journal_fuzzer.o \
	$(OBJ_DIR)/fuzz/standalone_main.o

.PHONY: all test demo tui cli scenarios replay-demo benchmark pipeline-benchmark fuzz-long format format-check lint \
	sanitize tsan coverage cmake-configure cmake-build clean

all: $(DEMO_BIN) $(TUI_BIN) $(REPLAY_BIN) $(CLI_BIN) $(CLI_TEST_BIN) $(FIX_GATEWAY_BIN) $(FIX_CLIENT_BIN) $(FIX_TEST_BIN) \
	$(FIX_FUZZ_BIN) $(ITCH_BIN) $(ITCH_TEST_BIN) $(ITCH_FUZZ_BIN) $(SIM_TEST_BIN) $(JOURNAL_TEST_BIN) $(PIPELINE_TEST_BIN) $(BENCH_BIN) \
	$(PIPELINE_BENCH_BIN) $(TEST_BIN) $(DIFF_TEST_BIN) $(FUZZ_BIN) $(JOURNAL_FUZZ_BIN) $(BENCH_TEST_BIN) $(INDEX_TEST_BIN) \
	$(ALLOC_TEST_BIN) $(MARKET_DATA_TEST_BIN)

# Each object also emits a .d file listing the headers it includes, so header edits trigger rebuilds.
$(OBJ_DIR)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(DEPFLAGS) -c $< -o $@

$(DEMO_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/demo_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

# A fused multiply-add rounds differently from a separate multiply and add, so the simulator disables contraction to
# replay a seed identically on every CPU.
$(OBJ_DIR)/src/simulator.o: CXXFLAGS += -ffp-contract=off

$(TUI_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/src/simulator.o $(OBJ_DIR)/tools/tui_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(SIM_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/src/simulator.o $(OBJ_DIR)/tests/simulator_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(REPLAY_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/replay_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(CLI_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/cli_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(CLI_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/cli_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(FIX_GATEWAY_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/fix_gateway_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(FIX_CLIENT_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tools/fix_client_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(FIX_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/fix_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(FIX_FUZZ_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/fuzz/fix_fuzzer.o $(OBJ_DIR)/fuzz/standalone_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(ITCH_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/src/simulator.o $(OBJ_DIR)/tools/itch_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(ITCH_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/src/simulator.o $(OBJ_DIR)/tests/itch_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(ITCH_FUZZ_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/fuzz/itch_fuzzer.o $(OBJ_DIR)/fuzz/standalone_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(JOURNAL_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/src/simulator.o $(OBJ_DIR)/tests/journal_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(PIPELINE_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/pipeline_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(PIPELINE_BENCH_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/bench/pipeline_main.o $(OBJ_DIR)/bench/workload.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(BENCH_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/bench/benchmark_main.o $(OBJ_DIR)/bench/workload.o \
		$(OBJ_DIR)/bench/baseline/map_order_book.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/order_book_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(DIFF_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/differential_tests.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

$(MARKET_DATA_TEST_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/tests/market_data_tests.o
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

$(JOURNAL_FUZZ_BIN): $(ENGINE_OBJ) $(OBJ_DIR)/fuzz/journal_fuzzer.o $(OBJ_DIR)/fuzz/standalone_main.o
	$(CXX) $(CXXFLAGS) $^ -o $@ $(LDFLAGS)

test: $(TEST_BIN) $(DIFF_TEST_BIN) $(FUZZ_BIN) $(JOURNAL_FUZZ_BIN) $(BENCH_TEST_BIN) $(INDEX_TEST_BIN) \
		$(ALLOC_TEST_BIN) $(MARKET_DATA_TEST_BIN) $(SIM_TEST_BIN) $(JOURNAL_TEST_BIN) $(PIPELINE_TEST_BIN) \
		$(CLI_TEST_BIN) $(FIX_TEST_BIN) $(FIX_FUZZ_BIN) $(ITCH_BIN) $(ITCH_TEST_BIN) $(ITCH_FUZZ_BIN) replay-demo scenarios
	./$(TEST_BIN)
	./$(CLI_TEST_BIN)
	./$(FIX_TEST_BIN)
	./$(FIX_FUZZ_BIN) --iterations 3000
	./$(ITCH_TEST_BIN)
	./$(ITCH_FUZZ_BIN) --iterations 2000
	./$(ITCH_BIN) generate $(BUILD_DIR)/itch-smoke.itch --minutes 1 --verify
	./$(ITCH_BIN) replay $(BUILD_DIR)/itch-smoke.itch --symbol ACME
	./$(SIM_TEST_BIN)
	./$(JOURNAL_TEST_BIN)
	./$(PIPELINE_TEST_BIN)
	./$(MARKET_DATA_TEST_BIN)
	./$(INDEX_TEST_BIN)
	./$(ALLOC_TEST_BIN)
	./$(BENCH_TEST_BIN)
	./$(DIFF_TEST_BIN)
	./$(FUZZ_BIN) --iterations 500
	./$(JOURNAL_FUZZ_BIN) --iterations 3000

# Records a simulated session with the TUI, then replays its journal and checks for the same state hash.
replay-demo: $(TUI_BIN) $(REPLAY_BIN)
	./$(TUI_BIN) --seed 5 --frames 300 --speed 8 --no-sleep --headless --journal $(BUILD_DIR)/session.tmj \
		| tee $(BUILD_DIR)/session.out
	./$(REPLAY_BIN) $(BUILD_DIR)/session.tmj --depth 3 \
		--expect-hash $$(sed -n 's/^state hash //p' $(BUILD_DIR)/session.out)

# Longer randomized soak: 2,000 seeds x 5,000 operations against the reference book.
fuzz-long: $(DIFF_TEST_BIN) $(FUZZ_BIN)
	./$(DIFF_TEST_BIN) --seeds 2000 --ops 5000
	./$(FUZZ_BIN) --iterations 20000

demo: $(DEMO_BIN)
	./$(DEMO_BIN)

tui: $(TUI_BIN)
	./$(TUI_BIN)

cli: $(CLI_BIN)
	./$(CLI_BIN)

# Every scenario's transcript must match its golden .expected file. After an intended change, regenerate them with
# ./build/tradematch_cli --update tests/scenarios/*.tm and review the diff.
scenarios: $(CLI_BIN)
	./$(CLI_BIN) --check $(SCENARIOS)

benchmark: $(BENCH_BIN)
	./$(BENCH_BIN)

# End-to-end latency through the gateway, matcher, and publisher threads at one million requests per second.
pipeline-benchmark: $(PIPELINE_BENCH_BIN)
	./$(PIPELINE_BENCH_BIN) --rate 1000000

format:
	$(CLANG_FORMAT) -i $(SOURCES)

format-check:
	$(CLANG_FORMAT) --dry-run --Werror $(SOURCES)

# The Python binding is linted when pybind11 is installed (pip install pybind11), since it compiles against its headers.
lint:
	@pybind11_dir=$$($(PYTHON) -m pybind11 --cmakedir 2>/dev/null); \
	if [ -n "$$pybind11_dir" ]; then \
		cmake -S . -B $(BUILD_DIR)/lint -DCMAKE_BUILD_TYPE=Debug -DTRADEMATCH_PYTHON=ON \
			"-DPython_EXECUTABLE=$$(command -v $(PYTHON))" "-Dpybind11_DIR=$$pybind11_dir" > /dev/null; \
	else \
		echo "pybind11 isn't installed, so python/ isn't linted"; \
		cmake -S . -B $(BUILD_DIR)/lint -DCMAKE_BUILD_TYPE=Debug -DTRADEMATCH_PYTHON=OFF > /dev/null; \
	fi && \
	git ls-files '*.cpp' ':!web/*' | { if [ -n "$$pybind11_dir" ]; then cat; else grep -v '^python/'; fi; } | \
		xargs -P $(JOBS) -n 1 $(CLANG_TIDY) -p $(BUILD_DIR)/lint --quiet $(TIDY_FLAGS)

sanitize:
	cmake -S . -B $(BUILD_DIR)/sanitize -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTRADEMATCH_WARNINGS_AS_ERRORS=ON \
		-DTRADEMATCH_SANITIZERS=address,undefined > /dev/null
	cmake --build $(BUILD_DIR)/sanitize --parallel $(JOBS)
	ctest --test-dir $(BUILD_DIR)/sanitize --output-on-failure

# Every test under ThreadSanitizer, which checks the pipeline's threads and the journal's I/O thread for data races.
tsan:
	cmake -S . -B $(BUILD_DIR)/tsan -DCMAKE_BUILD_TYPE=RelWithDebInfo -DTRADEMATCH_WARNINGS_AS_ERRORS=ON \
		-DTRADEMATCH_SANITIZERS=thread > /dev/null
	cmake --build $(BUILD_DIR)/tsan --parallel $(JOBS)
	TSAN_OPTIONS=halt_on_error=1 ctest --test-dir $(BUILD_DIR)/tsan --output-on-failure

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
