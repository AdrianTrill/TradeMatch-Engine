cmake_minimum_required(VERSION 3.16)

project(TradeMatchEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TRADEMATCH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(TRADEMATCH_COVERAGE "Instrument all targets for gcov-based coverage reports" OFF)
option(TRADEMATCH_LIBFUZZER "Build the coverage-guided libFuzzer target (requires LLVM clang)" OFF)
set(TRADEMATCH_SANITIZERS "" CACHE STRING "Sanitizers for all targets, e.g. address,undefined or thread")

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
    target_compile_options(tradematch_project_options INTERFACE --coverage)
    target_link_options(tradematch_project_options INTERFACE --coverage)
endif()

set(TRADEMATCH_ENGINE_SOURCES
    src/formatting.cpp
    src/order_book.cpp
)

add_library(tradematch_engine ${TRADEMATCH_ENGINE_SOURCES})
target_include_directories(tradematch_engine PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(tradematch_engine PRIVATE tradematch_project_options)

add_executable(tradematch_demo tools/demo_main.cpp)
target_link_libraries(tradematch_demo PRIVATE tradematch_engine tradematch_project_options)

add_executable(tradematch_benchmark tools/benchmark_main.cpp)
target_link_libraries(tradematch_benchmark PRIVATE tradematch_engine tradematch_project_options)

include(CTest)

if(BUILD_TESTING)
    add_executable(tradematch_tests tests/order_book_tests.cpp)
    target_link_libraries(tradematch_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_tests COMMAND tradematch_tests)

    add_executable(tradematch_differential_tests tests/differential_tests.cpp)
    target_include_directories(tradematch_differential_tests PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_differential_tests PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_differential_tests COMMAND tradematch_differential_tests)

    # The fuzz target also builds with a plain driver, so every toolchain compiles and smoke-tests it.
    add_executable(tradematch_fuzz_standalone fuzz/order_book_fuzzer.cpp fuzz/standalone_main.cpp)
    target_include_directories(tradematch_fuzz_standalone PRIVATE ${PROJECT_SOURCE_DIR}/tests)
    target_link_libraries(tradematch_fuzz_standalone PRIVATE tradematch_engine tradematch_project_options)
    add_test(NAME tradematch_fuzz_smoke COMMAND tradematch_fuzz_standalone --iterations 500)
endif()

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
