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

# Standalone Asio, not Boost.Asio (Spec 007 plan decision 2): header-only, FetchContent-able in
# seconds, no multi-minute Boost clone on a clean machine -- the one-command-build promise wins.
# It ships as a plain source tree (no CMakeLists of its own for the standalone flavor), so it is
# wired up as an INTERFACE target by hand below rather than via FetchContent_MakeAvailable.
FetchContent_Declare(asio
    GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
    GIT_TAG        asio-1-30-2
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(googletest googlebenchmark hdrhistogram asio)

add_library(velox_asio INTERFACE)
target_include_directories(velox_asio SYSTEM INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(velox_asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
find_package(Threads REQUIRED)
target_link_libraries(velox_asio INTERFACE Threads::Threads)

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
# Sequencer / journal / recovery (Spec 006). Header-only; this INTERFACE target exists so
# consumers declare the dependency explicitly rather than relying on velox_engine's include dirs
# leaking the right headers by accident.
# ---------------------------------------------------------------------------------------------
add_library(velox_sequencer INTERFACE)
target_link_libraries(velox_sequencer INTERFACE velox_engine)
target_include_directories(velox_sequencer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

# ---------------------------------------------------------------------------------------------
# Wire protocol (Spec 007 T1). Deliberately asio-free -- no I/O, no sockets -- so it can be
# fuzzed and unit-tested standalone, and so a protocol-only change never triggers a network
# dependency rebuild.
# ---------------------------------------------------------------------------------------------
add_library(velox_protocol
    protocol/decoder.cpp
    protocol/encoder.cpp
)
target_include_directories(velox_protocol PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velox_protocol PUBLIC velox_engine)

# ---------------------------------------------------------------------------------------------
# Gateway (Spec 007 T2-T5): the binary protocol's TCP server. The only component that allocates
# freely and the only one that links asio -- off the hot path entirely.
# ---------------------------------------------------------------------------------------------
add_library(velox_gateway_lib
    gateway/session.cpp
)
target_include_directories(velox_gateway_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velox_gateway_lib PUBLIC velox_protocol velox_sequencer velox_asio)

# ---------------------------------------------------------------------------------------------
# Market data (Spec 008): L2/L3 + trade-tick feed. Off the hot path entirely -- allocates and
# logs freely, per CLAUDE.md. feed_server.hpp links asio; book_mirror/publisher do not, so a
# publisher-only change (no wire format change) never triggers a network dependency rebuild.
# ---------------------------------------------------------------------------------------------
add_library(velox_marketdata
    marketdata/feed_encoder.cpp
    marketdata/feed_decoder.cpp
)
target_include_directories(velox_marketdata PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velox_marketdata PUBLIC velox_protocol velox_sequencer velox_asio)

# ---------------------------------------------------------------------------------------------
# Live visualizer (Spec 010): a strictly read-only downstream consumer of velox_marketdata's wire
# format, serving a browser ladder + latency histogram over a minimal hand-rolled WebSocket
# server. Off the hot path entirely -- does not link velox_engine at all.
# ---------------------------------------------------------------------------------------------
add_library(velox_viz_lib
    viz/ws_server.cpp
)
target_include_directories(velox_viz_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(velox_viz_lib PUBLIC velox_marketdata velox_asio)
target_compile_definitions(velox_viz_lib PUBLIC
    VELOX_VIZ_DEFAULT_ASSETS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/viz/web"
)

# ---------------------------------------------------------------------------------------------
# Tier-3 audit tier (Spec 012): a derived, queryable Postgres projection of the journal, built by
# a SEPARATE process (velox_auditd) that reads journal files off disk. OFF by default -- this is
# the first option() in the repo, and it stays that way deliberately: everything above this line
# builds with zero external dependencies beyond what FetchContent pulls, and that promise does
# not change unless a developer explicitly opts in here. See constitution.md CON-8 (amended
# 2026-08-02, progress_report.md [018]) and .claude/plans/012-postgres-audit-tier.md.
# ---------------------------------------------------------------------------------------------
option(VELOX_BUILD_AUDIT_TIER "Build the Tier-3 Postgres audit tier (Spec 012)" OFF)
if(VELOX_BUILD_AUDIT_TIER)
    find_package(PostgreSQL REQUIRED)  # clear configure-time failure if libpq is absent
    add_library(velox_audit
        audit/pg_writer.cpp
    )
    target_include_directories(velox_audit PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(velox_audit
        PUBLIC velox_engine velox_sequencer
        PRIVATE PostgreSQL::PostgreSQL
    )
endif()

# ---------------------------------------------------------------------------------------------
# Tests. CTest labels are fixed by NFR-32: unit, replay, invariant, bench, alloc_check.
# recovery (Spec 006) is a new label added to that set.
# ---------------------------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)
add_subdirectory(benchmark)
add_subdirectory(apps)
