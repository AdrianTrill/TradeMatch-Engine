cmake_minimum_required(VERSION 3.16)

project(TradeMatchEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TRADEMATCH_ENGINE_SOURCES
    src/formatting.cpp
    src/order_book.cpp
)

add_library(tradematch_engine ${TRADEMATCH_ENGINE_SOURCES})

target_include_directories(tradematch_engine
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
)

target_compile_options(tradematch_engine
    PRIVATE
        $<$<CXX_COMPILER_ID:MSVC>:/W4 /permissive->
        $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall -Wextra -Wpedantic -Wshadow>
)

add_executable(tradematch_demo tools/demo_main.cpp)
target_link_libraries(tradematch_demo PRIVATE tradematch_engine)

add_executable(tradematch_benchmark tools/benchmark_main.cpp)
target_link_libraries(tradematch_benchmark PRIVATE tradematch_engine)

include(CTest)

if(BUILD_TESTING)
    add_executable(tradematch_tests tests/order_book_tests.cpp)
    target_link_libraries(tradematch_tests PRIVATE tradematch_engine)
    add_test(NAME tradematch_tests COMMAND tradematch_tests)

    add_executable(tradematch_differential_tests tests/differential_tests.cpp)
    target_include_directories(tradematch_differential_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_differential_tests PRIVATE tradematch_engine)
    add_test(NAME tradematch_differential_tests COMMAND tradematch_differential_tests)

    # The fuzz target also builds with a plain driver, so every toolchain compiles and smoke-tests it.
    add_executable(tradematch_fuzz_standalone fuzz/order_book_fuzzer.cpp fuzz/standalone_main.cpp)
    target_include_directories(tradematch_fuzz_standalone PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_fuzz_standalone PRIVATE tradematch_engine)
    add_test(NAME tradematch_fuzz_smoke COMMAND tradematch_fuzz_standalone --iterations 500)
endif()

option(TRADEMATCH_LIBFUZZER "Build the coverage-guided libFuzzer target (requires LLVM clang)" OFF)

if(TRADEMATCH_LIBFUZZER)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "TRADEMATCH_LIBFUZZER requires clang with libFuzzer support")
    endif()

    # Engine sources are compiled into the fuzzer so libFuzzer receives coverage feedback from the matching code.
    add_executable(tradematch_fuzzer fuzz/order_book_fuzzer.cpp ${TRADEMATCH_ENGINE_SOURCES})
    target_include_directories(tradematch_fuzzer PRIVATE ${PROJECT_SOURCE_DIR}/include ${PROJECT_SOURCE_DIR}/tests)
    target_compile_options(tradematch_fuzzer PRIVATE -g -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
    target_link_options(tradematch_fuzzer PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
