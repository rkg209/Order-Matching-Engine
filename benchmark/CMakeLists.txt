add_executable(velox_bench velox_bench.cpp)
target_link_libraries(velox_bench PRIVATE velox_engine benchmark::benchmark hdr_histogram_static)
target_include_directories(velox_bench PRIVATE ${hdrhistogram_SOURCE_DIR}/include
                                               ${CMAKE_CURRENT_SOURCE_DIR})

add_executable(velox_alloc_check velox_alloc_check.cpp)
target_link_libraries(velox_alloc_check PRIVATE velox_engine)

find_package(Threads REQUIRED)
add_executable(velox_ring_bench velox_ring_bench.cpp)
target_link_libraries(velox_ring_bench PRIVATE velox_engine hdr_histogram_static Threads::Threads)
target_include_directories(velox_ring_bench PRIVATE ${hdrhistogram_SOURCE_DIR}/include
                                                    ${CMAKE_CURRENT_SOURCE_DIR})

# --- Spec 009: the rate-driven load generator + regression gate ------------------------------
add_executable(velox_loadgen velox_loadgen.cpp)
target_link_libraries(velox_loadgen PRIVATE velox_gateway_lib velox_marketdata velox_sequencer
                                            hdr_histogram_static Threads::Threads)
target_include_directories(velox_loadgen PRIVATE ${hdrhistogram_SOURCE_DIR}/include
                                                 ${CMAKE_CURRENT_SOURCE_DIR})

# NFR-32: these labels must exist.
add_test(NAME bench COMMAND velox_bench --benchmark_min_time=0.1s)
set_tests_properties(bench PROPERTIES LABELS "bench")

add_test(NAME ring_bench COMMAND velox_ring_bench)
set_tests_properties(ring_bench PROPERTIES LABELS "bench")

add_test(NAME alloc_check COMMAND velox_alloc_check)
set_tests_properties(alloc_check PROPERTIES LABELS "alloc_check")

# T6: the local regression gate -- runs the engine path at the baseline's implied rate and
# checks the result against the committed baseline. Authoritative locally; report-only in CI
# (see .github/workflows/ci.yml).
add_test(NAME bench_gate COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/bench_gate.sh)
set_tests_properties(bench_gate PROPERTIES LABELS "bench_gate")

# Spec 010 T8 / DoD: measures (not asserts) that attaching the visualizer does not regress the
# engine path's p99 -- two velox_loadgen runs, with and without velox_viz + a WS client attached.
add_test(NAME viz_decoupling COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/viz_decoupling.sh)
set_tests_properties(viz_decoupling PROPERTIES LABELS "viz_decoupling" TIMEOUT 300)

# Spec 011 T8 / DoD: measures (not asserts) that aggregate throughput scales with instrument
# (shard) count -- sweeps velox_loadgen --shards=N and writes benchmarks/shard_scaling.csv.
add_test(NAME shard_scaling COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/shard_scaling.sh)
set_tests_properties(shard_scaling PROPERTIES LABELS "shard_scaling" TIMEOUT 600)
