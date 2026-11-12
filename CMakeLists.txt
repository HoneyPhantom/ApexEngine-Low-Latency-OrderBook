cmake_minimum_required(VERSION 3.22)
project(ApexEngine LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-O3 -Wall -Wextra -Wpedantic -Werror)
    if(NOT (APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64"))
        add_compile_options(-march=native)
    endif()
endif()

find_package(Threads REQUIRED)

include_directories(include)

# -------------------------------------------------------------------
# Main Pipeline Application
# -------------------------------------------------------------------
add_library(runner_common INTERFACE)
target_link_libraries(runner_common INTERFACE  
    Threads::Threads
)

if(APPLE)
    target_link_libraries(runner_common INTERFACE "-framework CoreFoundation")
endif()

add_executable(apexBBO_runner apps/mainBBO.cpp)
add_executable(apexL1_runner apps/mainL1.cpp)
add_executable(apexL2_runner apps/mainL2.cpp)
add_executable(apexL3_runner apps/mainL3.cpp)

target_link_libraries(apexBBO_runner PRIVATE runner_common)
target_link_libraries(apexL1_runner PRIVATE runner_common)
target_link_libraries(apexL2_runner PRIVATE runner_common)
target_link_libraries(apexL3_runner PRIVATE runner_common)

# -------------------------------------------------------------------
# Automated Unit Testing Target
# -------------------------------------------------------------------
find_package(GTest REQUIRED)

add_executable(apex_tests tests/orderbook_test.cpp)
target_include_directories(apex_tests PRIVATE include)
target_link_libraries(apex_tests PRIVATE GTest::gtest_main)

# -------------------------------------------------------------------
# Google Benchmark Framework Integration
# -------------------------------------------------------------------
include(FetchContent)
FetchContent_Declare(
    googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.8.3
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googlebenchmark)

add_library(benchmark_common INTERFACE)
target_link_libraries(benchmark_common INTERFACE 
    benchmark::benchmark 
    Threads::Threads
)

if(APPLE)
    target_link_libraries(benchmark_common INTERFACE "-framework CoreFoundation")
endif()

add_executable(apexBBO_benchmark benchmarks/BBO_book_benchmark.cpp)
add_executable(apexL1_benchmark benchmarks/L1_book_benchmark.cpp)
add_executable(apexL2_benchmark benchmarks/L2_book_benchmark.cpp)
add_executable(apexL3_benchmark benchmarks/L3_book_benchmark.cpp)

target_link_libraries(apexBBO_benchmark PRIVATE benchmark_common)
target_link_libraries(apexL1_benchmark PRIVATE benchmark_common)
target_link_libraries(apexL2_benchmark PRIVATE benchmark_common)
target_link_libraries(apexL3_benchmark PRIVATE benchmark_common)