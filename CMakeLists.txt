cmake_minimum_required(VERSION 3.16)

project(TradeMatchEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TRADEMATCH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TRADEMATCH_COVERAGE "Instrument all targets for gcov-based coverage reports" OFF)
option(TRADEMATCH_LIBFUZZER "Build the coverage-guided libFuzzer target (requires LLVM clang)" OFF)
option(TRADEMATCH_PYTHON "Build the Python extension module (requires pybind11); pip install . turns it on" OFF)
set(TRADEMATCH_SANITIZERS "" CACHE STRING "Sanitizers for all targets, e.g. address,undefined or thread")

# A Python extension is a shared library, so everything linked into it must be position independent.
if(TRADEMATCH_PYTHON)
    set(CMAKE_POSITION_INDEPENDENT_CODE ON)
endif()

# Compile settings shared by every target in the project.
add_library(tradematch_project_options INTERFACE)

target_compile_options(tradematch_project_options
    INTERFACE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wshadow>
)

if(TRADEMATCH_WARNINGS_AS_ERRORS)
    target_compile_options(tradematch_project_options INTERFACE $<IF:$<CXX_COMPILER_ID:MSVC>,/WX,-Werror>)
endif()

# Emscripten turns off catching C++ exceptions by default, and the engine reports bad input by throwing, so the
# WebAssembly build uses the browsers' native exception handling, in every object it links.
if(EMSCRIPTEN)
    add_compile_options(-fwasm-exceptions)
    add_link_options(-fwasm-exceptions)
endif()

if(TRADEMATCH_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "TRADEMATCH_SANITIZERS is only supported with GCC and Clang")
    endif()
    target_compile_options(tradematch_project_options
        INTERFACE -fsanitize=${TRADEMATCH_SANITIZERS} -fno-sanitize-recover=all -fno-omit-frame-pointer -g)
    target_link_options(tradematch_project_options INTERFACE -fsanitize=${TRADEMATCH_SANITIZERS})
endif()

if(TRADEMATCH_COVERAGE)
    if(MSVC)
        message(FATAL_ERROR "TRADEMATCH_COVERAGE is only supported with GCC and Clang")
    endif()
    # Several threads bump the same counters (the pipeline's polling loops above all), so the updates must be atomic;
    # otherwise increments are lost and gcov can even read negative hit counts.
    target_compile_options(tradematch_project_options INTERFACE --coverage -fprofile-update=atomic)
    target_link_options(tradematch_project_options INTERFACE --coverage)
endif()

set(TRADEMATCH_ENGINE_SOURCES
    src/cli.cpp
    src/codec.cpp
    src/file_io.cpp
    src/fix.cpp
    src/fix_gateway.cpp
    src/formatting.cpp
    src/itch.cpp
    src/itch_feed.cpp
    src/journal.cpp
    src/market_data.cpp
    src/matching_engine.cpp
    src/order_book.cpp
    src/pipeline.cpp
    src/snapshot.cpp
    src/socket.cpp
)

find_package(Threads REQUIRED)

add_library(tradematch_engine ${TRADEMATCH_ENGINE_SOURCES})
target_include_directories(tradematch_engine PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(tradematch_engine PUBLIC Threads::Threads PRIVATE tradematch_project_options)
if(WIN32)
    target_link_libraries(tradematch_engine PUBLIC ws2_32)
endif()

add_library(tradematch_sim src/simulator.cpp)
target_link_libraries(tradematch_sim PUBLIC tradematch_engine PRIVATE tradematch_project_options)
# A fused multiply-add rounds differently from a separate multiply and add, so the simulator disables contraction to
# replay a seed identically on every CPU. MSVC does not contract by default.
target_compile_options(tradematch_sim PRIVATE $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-ffp-contract=off>)

add_executable(tradematch_tui tools/tui_main.cpp)
target_link_libraries(tradematch_tui PRIVATE tradematch_sim tradematch_project_options)

# The browser playground: the simulator and the command line compiled to WebAssembly, plus the page that drives them,
# assembled in site/ for GitHub Pages. Built with emcmake; see .github/workflows/pages.yml.
if(EMSCRIPTEN)
    set(TRADEMATCH_SITE_DIR ${CMAKE_CURRENT_BINARY_DIR}/site)
    add_executable(tradematch_wasm web/playground.cpp)
    target_link_libraries(tradematch_wasm PRIVATE tradematch_sim tradematch_project_options)
    target_link_options(tradematch_wasm PRIVATE
        -lembind
        -sMODULARIZE=1
        -sEXPORT_ES6=1
        -sEXPORT_NAME=createTradeMatch
        -sALLOW_MEMORY_GROWTH=1
        -sENVIRONMENT=web,node)
    set_target_properties(tradematch_wasm PROPERTIES OUTPUT_NAME tradematch RUNTIME_OUTPUT_DIRECTORY ${TRADEMATCH_SITE_DIR})
    # The page, its scripts, and the scenario scripts its command line offers as examples, copied fresh on every build.
    file(GLOB TRADEMATCH_WEB_ASSETS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/web/*.html ${PROJECT_SOURCE_DIR}/web/*.css
         ${PROJECT_SOURCE_DIR}/web/*.svg ${PROJECT_SOURCE_DIR}/web/*.png)
    file(GLOB TRADEMATCH_WEB_SCRIPTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/web/js/*.js)
    file(GLOB TRADEMATCH_WEB_SCENARIOS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/scenarios/*.tm)
    add_custom_target(tradematch_site
        COMMAND ${CMAKE_COMMAND} -E copy ${TRADEMATCH_WEB_ASSETS} ${TRADEMATCH_SITE_DIR}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TRADEMATCH_SITE_DIR}/js
        COMMAND ${CMAKE_COMMAND} -E copy ${TRADEMATCH_WEB_SCRIPTS} ${TRADEMATCH_SITE_DIR}/js
        COMMAND ${CMAKE_COMMAND} -E make_directory ${TRADEMATCH_SITE_DIR}/scenarios
        COMMAND ${CMAKE_COMMAND} -E copy ${TRADEMATCH_WEB_SCENARIOS} ${TRADEMATCH_SITE_DIR}/scenarios
        DEPENDS tradematch_wasm)
endif()

add_executable(tradematch_itch tools/itch_main.cpp)
target_link_libraries(tradematch_itch PRIVATE tradematch_sim tradematch_project_options)

add_executable(tradematch_fix_gateway tools/fix_gateway_main.cpp)
target_link_libraries(tradematch_fix_gateway PRIVATE tradematch_engine tradematch_project_options)

add_executable(tradematch_fix_client tools/fix_client_main.cpp)
target_link_libraries(tradematch_fix_client PRIVATE tradematch_engine tradematch_project_options)

add_executable(tradematch_cli tools/cli_main.cpp)
target_link_libraries(tradematch_cli PRIVATE tradematch_engine tradematch_project_options)

add_executable(tradematch_replay tools/replay_main.cpp)
target_link_libraries(tradematch_replay PRIVATE tradematch_engine tradematch_project_options)

add_executable(tradematch_demo tools/demo_main.cpp)
target_link_libraries(tradematch_demo PRIVATE tradematch_engine tradematch_project_options)

add_library(tradematch_bench_support STATIC bench/workload.cpp bench/baseline/map_order_book.cpp)
target_include_directories(tradematch_bench_support PUBLIC ${PROJECT_SOURCE_DIR}/bench)
target_link_libraries(tradematch_bench_support PUBLIC tradematch_engine PRIVATE tradematch_project_options)

add_executable(tradematch_benchmark bench/benchmark_main.cpp)
target_link_libraries(tradematch_benchmark PRIVATE tradematch_bench_support tradematch_project_options)

add_executable(tradematch_pipeline_benchmark bench/pipeline_main.cpp)
target_link_libraries(tradematch_pipeline_benchmark PRIVATE tradematch_bench_support tradematch_project_options)

include(CTest)

if(BUILD_TESTING)
    add_executable(tradematch_tests tests/order_book_tests.cpp)
    target_link_libraries(tradematch_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_tests COMMAND tradematch_tests)

    add_executable(tradematch_market_data_tests tests/market_data_tests.cpp)
    target_include_directories(tradematch_market_data_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_market_data_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_market_data_tests COMMAND tradematch_market_data_tests)

    add_executable(tradematch_simulator_tests tests/simulator_tests.cpp)
    target_link_libraries(tradematch_simulator_tests PRIVATE tradematch_sim tradematch_project_options)
    add_test(NAME tradematch_simulator_tests COMMAND tradematch_simulator_tests)
    add_test(NAME tradematch_tui_smoke COMMAND tradematch_tui --frames 60 --speed 4 --no-sleep --headless)

    add_executable(tradematch_journal_tests tests/journal_tests.cpp)
    target_include_directories(tradematch_journal_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_journal_tests PRIVATE tradematch_sim tradematch_project_options)
    add_test(NAME tradematch_journal_tests COMMAND tradematch_journal_tests)
    # Records a simulated session through the TUI, replays the journal with the replay tool, and checks that both
    # report the same state hash.
    add_test(NAME tradematch_replay_roundtrip
             COMMAND ${CMAKE_COMMAND} -DTUI=$<TARGET_FILE:tradematch_tui> -DREPLAY=$<TARGET_FILE:tradematch_replay>
                     -DJOURNAL=${CMAKE_CURRENT_BINARY_DIR}/replay_roundtrip.tmj -P ${PROJECT_SOURCE_DIR}/cmake/replay_roundtrip.cmake)

    add_executable(tradematch_itch_tests tests/itch_tests.cpp)
    target_include_directories(tradematch_itch_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_itch_tests PRIVATE tradematch_sim tradematch_project_options)
    add_test(NAME tradematch_itch_tests COMMAND tradematch_itch_tests)

    # Writes a simulated day as an ITCH file, verifies it rebuilds the engine's books, and replays it.
    add_test(NAME tradematch_itch_roundtrip
             COMMAND ${CMAKE_COMMAND} -DITCH=$<TARGET_FILE:tradematch_itch>
                     -DFEED=${CMAKE_CURRENT_BINARY_DIR}/itch_roundtrip.itch -P ${PROJECT_SOURCE_DIR}/cmake/itch_roundtrip.cmake)

    add_executable(tradematch_fix_tests tests/fix_tests.cpp)
    target_link_libraries(tradematch_fix_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_fix_tests COMMAND tradematch_fix_tests)

    add_executable(tradematch_cli_tests tests/cli_tests.cpp)
    target_link_libraries(tradematch_cli_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_cli_tests COMMAND tradematch_cli_tests)

    # Each scenario script's transcript must match its golden .expected file exactly.
    file(GLOB TRADEMATCH_SCENARIOS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/tests/scenarios/*.tm)
    foreach(scenario IN LISTS TRADEMATCH_SCENARIOS)
        get_filename_component(scenario_name ${scenario} NAME_WE)
        add_test(NAME tradematch_scenario_${scenario_name} COMMAND tradematch_cli --check ${scenario})
    endforeach()

    add_executable(tradematch_pipeline_tests tests/pipeline_tests.cpp)
    target_include_directories(tradematch_pipeline_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_pipeline_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_pipeline_tests COMMAND tradematch_pipeline_tests)

    add_executable(tradematch_order_index_tests tests/order_index_tests.cpp)
    target_link_libraries(tradematch_order_index_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_order_index_tests COMMAND tradematch_order_index_tests)

    add_executable(tradematch_differential_tests tests/differential_tests.cpp)
    target_include_directories(tradematch_differential_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_differential_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_differential_tests COMMAND tradematch_differential_tests)

    # The fuzz target also builds with a plain driver, so every toolchain compiles and smoke-tests it.
    add_executable(tradematch_fuzz_standalone fuzz/order_book_fuzzer.cpp fuzz/standalone_main.cpp)
    target_include_directories(tradematch_fuzz_standalone PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_fuzz_standalone PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_fuzz_smoke COMMAND tradematch_fuzz_standalone --iterations 500)

    add_executable(tradematch_journal_fuzz_standalone fuzz/journal_fuzzer.cpp fuzz/standalone_main.cpp)
    target_link_libraries(tradematch_journal_fuzz_standalone PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_journal_fuzz_smoke COMMAND tradematch_journal_fuzz_standalone --iterations 3000)

    add_executable(tradematch_itch_fuzz_standalone fuzz/itch_fuzzer.cpp fuzz/standalone_main.cpp)
    target_include_directories(tradematch_itch_fuzz_standalone PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_itch_fuzz_standalone PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_itch_fuzz_smoke COMMAND tradematch_itch_fuzz_standalone --iterations 2000)

    add_executable(tradematch_fix_fuzz_standalone fuzz/fix_fuzzer.cpp fuzz/standalone_main.cpp)
    target_link_libraries(tradematch_fix_fuzz_standalone PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_fix_fuzz_smoke COMMAND tradematch_fix_fuzz_standalone --iterations 3000)
    # Order round trips over loopback TCP against an in-process gateway.
    add_test(NAME tradematch_fix_client_bench_smoke COMMAND tradematch_fix_client --bench 2000)

    add_executable(tradematch_bench_tests tests/bench_tests.cpp)
    target_link_libraries(tradematch_bench_tests PRIVATE tradematch_bench_support tradematch_project_options)
    add_test(NAME tradematch_bench_tests COMMAND tradematch_bench_tests)

    # Replaces global operator new to prove the request path never allocates. Sanitizers install their own
    # allocator hooks, so the check only runs in plain builds.
    if(NOT TRADEMATCH_SANITIZERS)
        add_executable(tradematch_allocation_tests tests/allocation_tests.cpp)
        target_link_libraries(tradematch_allocation_tests PRIVATE tradematch_bench_support tradematch_project_options)
        add_test(NAME tradematch_allocation_tests COMMAND tradematch_allocation_tests)
    endif()

    add_test(NAME tradematch_benchmark_smoke
             COMMAND tradematch_benchmark --ops 20000 --warmup 2000 --prefill 1000 --no-distribution)
    # Also checks that the threaded pipeline's book matches a single-threaded replay.
    add_test(NAME tradematch_pipeline_benchmark_smoke
             COMMAND tradematch_pipeline_benchmark --ops 50000 --warmup 5000 --prefill 1000 --share-cores)
endif()

if(TRADEMATCH_LIBFUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "TRADEMATCH_LIBFUZZER requires clang with libFuzzer support")
    endif()

    # Engine sources are compiled into the fuzzers so libFuzzer receives coverage feedback from the engine code.
    function(tradematch_add_fuzzer target source)
        add_executable(${target} ${source} ${TRADEMATCH_ENGINE_SOURCES})
        target_include_directories(${target} PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests)
        target_compile_options(${target} PRIVATE -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
        target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
        target_link_libraries(${target} PRIVATE Threads::Threads)
    endfunction()
    tradematch_add_fuzzer(tradematch_fuzzer fuzz/order_book_fuzzer.cpp)
    tradematch_add_fuzzer(tradematch_journal_fuzzer fuzz/journal_fuzzer.cpp)
    tradematch_add_fuzzer(tradematch_fix_fuzzer fuzz/fix_fuzzer.cpp)
    tradematch_add_fuzzer(tradematch_itch_fuzzer fuzz/itch_fuzzer.cpp)
    if(WIN32)
        target_link_libraries(tradematch_fuzzer PRIVATE ws2_32)
    endif()
endif()

# The Python module, built by scikit-build-core from pyproject.toml.
if(TRADEMATCH_PYTHON)
    find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)
    find_package(pybind11 CONFIG REQUIRED)
    pybind11_add_module(_tradematch python/src/bindings.cpp)
    target_link_libraries(_tradematch PRIVATE tradematch_sim tradematch_project_options)
    install(TARGETS _tradematch DESTINATION tradematch)
endif()
