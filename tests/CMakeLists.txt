include(GoogleTest)
find_package(Threads REQUIRED)

# --- unit ------------------------------------------------------------------------------------
add_executable(velox_unit_tests
    unit/object_pool_test.cpp
    unit/price_level_test.cpp
    unit/level_map_test.cpp
    unit/order_id_map_test.cpp
    unit/order_book_test.cpp
    unit/cache_test.cpp
    unit/spsc_ring_test.cpp
    unit/matching_thread_test.cpp
)
target_link_libraries(velox_unit_tests PRIVATE velox_engine GTest::gtest_main Threads::Threads)
gtest_discover_tests(velox_unit_tests PROPERTIES LABELS "unit")

# Spec 005 T0: the SPSC ring's two-thread strict-sequence test, rebuilt under ThreadSanitizer.
# This is the one place in the project where a data race is possible at all, so it is the one
# place the sanitizer earns its (considerable) runtime cost -- kept as its own target rather
# than sanitizing the whole unit suite.
add_executable(velox_spsc_tsan_test unit/spsc_ring_tsan_test.cpp)
target_link_libraries(velox_spsc_tsan_test PRIVATE velox_engine GTest::gtest_main Threads::Threads)
target_compile_options(velox_spsc_tsan_test PRIVATE -fsanitize=thread -g -O1)
target_link_options(velox_spsc_tsan_test PRIVATE -fsanitize=thread)
add_test(NAME SpscRing.Tsan COMMAND velox_spsc_tsan_test)
set_tests_properties(SpscRing.Tsan PROPERTIES LABELS "tsan")

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

# --- recovery (Spec 006: journal/snapshot/sequencer round trips + RecoveryManager) -----------
add_executable(velox_recovery_tests
    recovery/recovery_test.cpp
)
target_link_libraries(velox_recovery_tests PRIVATE velox_sequencer GTest::gtest_main Threads::Threads)
gtest_discover_tests(velox_recovery_tests PROPERTIES LABELS "recovery")

# SIGKILL-a-real-process suite: forks/execs the actual velox_live binary, so it must be built
# first and its path handed in as a compile definition. Generous timeout -- it forks two real
# processes and does real fsync I/O.
add_executable(velox_recover_sigkill_test
    recovery/recover_sigkill_test.cpp
)
target_link_libraries(velox_recover_sigkill_test PRIVATE velox_sequencer GTest::gtest_main)
add_dependencies(velox_recover_sigkill_test velox_live)
target_compile_definitions(velox_recover_sigkill_test
    PRIVATE VELOX_LIVE_BIN="$<TARGET_FILE:velox_live>"
)
add_test(NAME Recovery.SigkillMidStream COMMAND velox_recover_sigkill_test)
set_tests_properties(Recovery.SigkillMidStream PROPERTIES LABELS "recovery" TIMEOUT 300)

# --- gateway (Spec 007: wire protocol + TCP gateway) ------------------------------------------
add_executable(velox_gateway_tests
    gateway/codec_test.cpp
    gateway/fragmentation_test.cpp
    gateway/hostile_test.cpp
    gateway/session_test.cpp
    gateway/routing_test.cpp
    gateway/backpressure_test.cpp
)
target_link_libraries(velox_gateway_tests PRIVATE velox_gateway_lib velox_protocol
                                                   GTest::gtest_main Threads::Threads)
target_include_directories(velox_gateway_tests PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(velox_gateway_tests PROPERTIES LABELS "gateway")

# Fuzz pass built with -fsanitize=address,undefined (NFR-27, DoD 4) -- a buffer overrun or UB in
# the decoder must fail loudly here, not survive into production.
add_executable(velox_gateway_fuzz gateway/fuzz_test.cpp)
target_link_libraries(velox_gateway_fuzz PRIVATE velox_protocol GTest::gtest_main)
target_compile_options(velox_gateway_fuzz PRIVATE -fsanitize=address,undefined -g -O1)
target_link_options(velox_gateway_fuzz PRIVATE -fsanitize=address,undefined)
add_test(NAME Gateway.Fuzz COMMAND velox_gateway_fuzz)
set_tests_properties(Gateway.Fuzz PROPERTIES LABELS "gateway")

# Spawns the real velox_gateway binary, SIGKILLs it mid-stream, restarts it, and checks the
# recovered sequence -- same pattern as recovery/recover_sigkill_test.cpp.
add_executable(velox_gateway_e2e_test gateway/e2e_test.cpp)
target_link_libraries(velox_gateway_e2e_test PRIVATE velox_gateway_lib velox_protocol
                                                      GTest::gtest_main)
add_dependencies(velox_gateway_e2e_test velox_gateway)
target_compile_definitions(velox_gateway_e2e_test
    PRIVATE VELOX_GATEWAY_BIN="$<TARGET_FILE:velox_gateway>"
)
add_test(NAME Gateway.E2EKillRestart COMMAND velox_gateway_e2e_test)
set_tests_properties(Gateway.E2EKillRestart PROPERTIES LABELS "gateway" TIMEOUT 60)

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
