cmake_minimum_required(VERSION 3.16)

project(TradeMatchEngine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(tradematch_engine
    src/formatting.cpp
    src/order_book.cpp
)

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
endif()
