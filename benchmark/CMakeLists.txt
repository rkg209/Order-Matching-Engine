add_executable(velox_bench velox_bench.cpp)
target_link_libraries(velox_bench PRIVATE velox_engine benchmark::benchmark hdr_histogram_static)
target_include_directories(velox_bench PRIVATE ${hdrhistogram_SOURCE_DIR}/include)

add_executable(velox_alloc_check velox_alloc_check.cpp)
target_link_libraries(velox_alloc_check PRIVATE velox_engine)

# NFR-32: these labels must exist.
add_test(NAME bench COMMAND velox_bench --benchmark_min_time=0.1s)
set_tests_properties(bench PROPERTIES LABELS "bench")

add_test(NAME alloc_check COMMAND velox_alloc_check)
set_tests_properties(alloc_check PROPERTIES LABELS "alloc_check")
