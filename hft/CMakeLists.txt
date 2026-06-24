cmake_minimum_required(VERSION 3.20)
project(hft_sample CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Aggressive optimisation flags typical of HFT builds.
add_compile_options(
    -O3
    -march=native
    -funroll-loops
    -fno-omit-frame-pointer   # keep for perf profiling
    -Wall
    -Wextra
)

include_directories(include)

add_library(hft_core STATIC
    src/order_book.cpp
    src/market_data.cpp
    src/strategy.cpp
)

add_executable(hft_demo src/main.cpp)
target_link_libraries(hft_demo PRIVATE hft_core)
