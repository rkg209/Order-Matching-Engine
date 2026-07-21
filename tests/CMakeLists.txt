include(GoogleTest)

# --- unit ------------------------------------------------------------------------------------
add_executable(velox_unit_tests
    unit/object_pool_test.cpp
    unit/price_level_test.cpp
    unit/level_map_test.cpp
    unit/order_id_map_test.cpp
    unit/order_book_test.cpp
    unit/cache_test.cpp
)
target_link_libraries(velox_unit_tests PRIVATE velox_engine GTest::gtest_main)
gtest_discover_tests(velox_unit_tests PROPERTIES LABELS "unit")

# --- replay (golden scenarios, compared byte-for-byte) ---------------------------------------
add_executable(velox_replay_tests
    replay/replay_test.cpp
)
target_link_libraries(velox_replay_tests PRIVATE velox_engine GTest::gtest_main)
target_compile_definitions(velox_replay_tests
    PRIVATE VELOX_REPLAY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/replay"
)
gtest_discover_tests(velox_replay_tests PROPERTIES LABELS "replay")

# --- invariant (randomized property tests, asserted after every op) -------------------------
add_executable(velox_invariant_tests
    invariant/property_test.cpp
)
target_link_libraries(velox_invariant_tests PRIVATE velox_engine GTest::gtest_main)
gtest_discover_tests(velox_invariant_tests PROPERTIES LABELS "invariant")

# --- structural (mechanical proofs, not tests of behavior) -----------------------------------

# T1.2: engine/ and book/ headers must compile without exception support or RTTI. If a hot-path
# header needs either, this translation unit fails to build.
add_executable(velox_no_exceptions_tu structural/no_exceptions_tu.cpp)
target_link_libraries(velox_no_exceptions_tu PRIVATE velox_engine)
target_compile_options(velox_no_exceptions_tu PRIVATE -fno-exceptions -fno-rtti)
add_test(NAME Structural.NoExceptionsNoRtti COMMAND velox_no_exceptions_tu)
set_tests_properties(Structural.NoExceptionsNoRtti PROPERTIES LABELS "unit")

# T1.3: forbidden-construct gate over engine/ and book/ source. Run as a script rather than
# compiled, since it inspects source text rather than behavior.
find_package(Python3 COMPONENTS Interpreter REQUIRED)
add_test(NAME Structural.HotPathForbiddenConstructs
         COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/structural/hot_path_grep_test.py)
set_tests_properties(Structural.HotPathForbiddenConstructs PROPERTIES LABELS "unit")
