include(GoogleTest)

# --- unit ------------------------------------------------------------------------------------
add_executable(velox_unit_tests
    unit/object_pool_test.cpp
    unit/price_level_test.cpp
    unit/level_map_test.cpp
    unit/order_id_map_test.cpp
    unit/order_book_test.cpp
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
