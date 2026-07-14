cmake_minimum_required(VERSION 3.24)
project(velox_matching_engine LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# -DNDEBUG is NOT optional here. Overriding CMAKE_CXX_FLAGS_RELEASE drops CMake's default
# -DNDEBUG, which leaves assert() live and makes Google Benchmark (correctly) warn that it is
# timing a debug build. Benchmarking with assertions enabled measures the assertions.
# -g and frame pointers are kept: /profile needs them, and they cost nothing at -O3.
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -g -DNDEBUG -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

add_compile_options(-Wall -Wextra -Wpedantic)

# ---------------------------------------------------------------------------------------------
# Dependencies.
#
# FetchContent, deliberately: not vcpkg, not Conan. `cmake -B build` must work on a clean
# machine with nothing pre-installed, because "one command to build" is a promise this repo
# makes to anyone who clones it. The planning docs listed these libraries but never decided how
# they would be acquired.
# ---------------------------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.15.2
    GIT_SHALLOW    TRUE
)

set(BENCHMARK_ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(BENCHMARK_ENABLE_GTEST_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googlebenchmark
    GIT_REPOSITORY https://github.com/google/benchmark.git
    GIT_TAG        v1.9.0
    GIT_SHALLOW    TRUE
)

# HdrHistogram_c: coordinated-omission-free latency capture (NFR-4). Mandatory (CON-5).
set(HDR_HISTOGRAM_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(HDR_HISTOGRAM_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(HDR_LOG_REQUIRED OFF CACHE BOOL "" FORCE)
FetchContent_Declare(hdrhistogram
    GIT_REPOSITORY https://github.com/HdrHistogram/HdrHistogram_c.git
    GIT_TAG        0.11.8
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(googletest googlebenchmark hdrhistogram)

# ---------------------------------------------------------------------------------------------
# The engine.
#
# Header-mostly for now; the hot-path types are small and benefit from being inlined across
# translation units. Spec 004 revisits this if the profile says to.
# ---------------------------------------------------------------------------------------------
add_library(velox_engine
    engine/order_book.cpp
)
target_include_directories(velox_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(velox_engine PUBLIC cxx_std_20)

# ---------------------------------------------------------------------------------------------
# Tests. CTest labels are fixed by NFR-32: unit, replay, invariant, bench, alloc_check.
# ---------------------------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmark)
