cmake_minimum_required(VERSION 3.22)
project(ApexEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(CMAKE_COMPILER_IS_GNUCXX OR CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    add_compile_options(-O3 -march=native -Wall -Wextra -Wpedantic -Werror)
endif()

include_directories(include)

add_executable(apex_runner apps/main.cpp)

if(APPLE)
    target_link_libraries(apex_runner PRIVATE "-framework CoreFoundation")
endif()