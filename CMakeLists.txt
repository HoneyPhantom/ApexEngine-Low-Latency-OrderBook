cmake_minimum_required(VERSION 3.22)
project(ApexEngine LANGUAGES CXX)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic -Werror)
endif()

find_package(Threads REQUIRED)

include_directories(include)

# -------------------------------------------------------------------
# Main Pipeline Application
# -------------------------------------------------------------------
add_executable(apex_runner apps/main.cpp)
target_link_libraries(apex_runner PRIVATE Threads::Threads)
if(APPLE)
    target_link_libraries(apex_runner PRIVATE "-framework CoreFoundation")
endif()

# -------------------------------------------------------------------
# Automated Unit Testing Target
# -------------------------------------------------------------------
add_executable(apex_test tests/orderbook_test.cpp)
target_link_libraries(apex_test PRIVATE Threads::Threads)
if(APPLE)
    target_link_libraries(apex_test PRIVATE "-framework CoreFoundation")
endif()

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

add_executable(apex_benchmark benchmarks/book_benchmark.cpp)
target_link_libraries(apex_benchmark PRIVATE benchmark::benchmark Threads::Threads)
if(APPLE)
    target_link_libraries(apex_benchmark PRIVATE "-framework CoreFoundation")
endif()