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
    gateway/multi_instrument_routing_test.cpp
)
target_link_libraries(velox_gateway_tests PRIVATE velox_gateway_lib velox_protocol
                                                   GTest::gtest_main Threads::Threads)
target_include_directories(velox_gateway_tests PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(velox_gateway_tests PROPERTIES LABELS "gateway")

# Spec 011 T9.4: the routes_ race fix (gateway/gateway.hpp's ShardCtx::routesMutex), rebuilt
# under ThreadSanitizer -- same pattern as velox_spsc_tsan_test. Many concurrent sessions submit
# orders across multiple shards while the router threads concurrently route exec reports back,
# so registerRoute/eraseRoute (io thread) and routeExecReport/routeReject (router threads) race
# on each shard's routes_ map unless the mutex actually protects it.
add_executable(velox_gateway_routes_tsan_test gateway/routes_race_tsan_test.cpp)
target_link_libraries(velox_gateway_routes_tsan_test PRIVATE velox_gateway_lib velox_protocol
                                                             GTest::gtest_main Threads::Threads)
target_include_directories(velox_gateway_routes_tsan_test PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_options(velox_gateway_routes_tsan_test PRIVATE -fsanitize=thread -g -O1)
target_link_options(velox_gateway_routes_tsan_test PRIVATE -fsanitize=thread)
add_test(NAME Gateway.RoutesRaceTsan COMMAND velox_gateway_routes_tsan_test)
set_tests_properties(Gateway.RoutesRaceTsan PROPERTIES LABELS "tsan" TIMEOUT 120)

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

# --- marketdata (Spec 008: L2/L3 + trade-tick feed) -------------------------------------------
add_executable(velox_marketdata_tests
    marketdata/reconstruction_test.cpp
    marketdata/codec_test.cpp
    marketdata/slow_subscriber_test.cpp
    marketdata/feed_server_test.cpp
)
target_link_libraries(velox_marketdata_tests PRIVATE velox_marketdata velox_engine
                                                      GTest::gtest_main Threads::Threads)
target_include_directories(velox_marketdata_tests PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(velox_marketdata_tests PROPERTIES LABELS "marketdata")

# --- viz (Spec 010: the live visualizer) -------------------------------------------------------
add_executable(velox_viz_tests
    viz/ws_handshake_test.cpp
    viz/ladder_test.cpp
    viz/readonly_test.cpp
)
target_link_libraries(velox_viz_tests PRIVATE velox_viz_lib GTest::gtest_main Threads::Threads)
target_include_directories(velox_viz_tests PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(velox_viz_tests PROPERTIES LABELS "viz")

# Spawns the real velox_loadgen binary twice against a fixture journal -- must be built first,
# same pattern as velox_recover_sigkill_test / velox_gateway_e2e_test above.
add_executable(velox_viz_replay_determinism_test viz/replay_determinism_test.cpp)
target_link_libraries(velox_viz_replay_determinism_test PRIVATE velox_sequencer GTest::gtest_main)
add_dependencies(velox_viz_replay_determinism_test velox_loadgen)
target_compile_definitions(velox_viz_replay_determinism_test
    PRIVATE VELOX_LOADGEN_BIN="$<TARGET_FILE:velox_loadgen>"
)
add_test(NAME Viz.ReplayDeterminism COMMAND velox_viz_replay_determinism_test)
set_tests_properties(Viz.ReplayDeterminism PROPERTIES LABELS "viz" TIMEOUT 60)

# --- shard (Spec 011: multi-instrument sharding) -----------------------------------------------
add_executable(velox_shard_tests
    shard/determinism_test.cpp
    shard/isolation_test.cpp
)
target_link_libraries(velox_shard_tests PRIVATE velox_sequencer GTest::gtest_main Threads::Threads)
target_include_directories(velox_shard_tests PRIVATE ${CMAKE_SOURCE_DIR})
target_compile_definitions(velox_shard_tests
    PRIVATE VELOX_REPLAY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/replay"
)
gtest_discover_tests(velox_shard_tests PROPERTIES LABELS "shard" TIMEOUT 60)

# --- admin (Spec 013: REST admin API, revived at narrow read-only scope) -----------------------
add_executable(velox_admin_tests
    admin/hmac_test.cpp
    admin/base64url_test.cpp
    admin/jwt_test.cpp
    admin/routes_test.cpp
)
target_link_libraries(velox_admin_tests PRIVATE velox_admin GTest::gtest_main Threads::Threads)
target_include_directories(velox_admin_tests PRIVATE ${CMAKE_SOURCE_DIR})
gtest_discover_tests(velox_admin_tests PROPERTIES LABELS "admin")

# Spawns the real velox_adminctl binary against a journal directory produced by a real
# velox_gateway run -- same "drive the real binaries" pattern as gateway/e2e_test.cpp.
add_executable(velox_admin_e2e_test admin/e2e_test.cpp)
target_link_libraries(velox_admin_e2e_test PRIVATE velox_admin velox_gateway_lib velox_protocol
                                                    GTest::gtest_main Threads::Threads)
target_include_directories(velox_admin_e2e_test PRIVATE ${CMAKE_SOURCE_DIR})
add_dependencies(velox_admin_e2e_test velox_gateway velox_adminctl)
target_compile_definitions(velox_admin_e2e_test
    PRIVATE VELOX_GATEWAY_BIN="$<TARGET_FILE:velox_gateway>"
            VELOX_ADMINCTL_BIN="$<TARGET_FILE:velox_adminctl>"
)
add_test(NAME Admin.E2E COMMAND velox_admin_e2e_test)
set_tests_properties(Admin.E2E PROPERTIES LABELS "admin" TIMEOUT 60)

# --- bench (Spec 009: "test the test" -- the CO-correction proof) ----------------------------
add_executable(velox_co_correction_test
    bench/co_correction_test.cpp
)
target_link_libraries(velox_co_correction_test PRIVATE velox_gateway_lib velox_marketdata
                                                       velox_sequencer hdr_histogram_static
                                                       GTest::gtest_main Threads::Threads)
target_include_directories(velox_co_correction_test PRIVATE ${CMAKE_SOURCE_DIR}/benchmark
                                                            ${hdrhistogram_SOURCE_DIR}/include)
gtest_discover_tests(velox_co_correction_test PROPERTIES LABELS "co" TIMEOUT 120)

# --- audit (Spec 012: Tier-3 Postgres audit tier) ----------------------------------------------
# Same guard as velox_audit/velox_auditd (CMakeLists.txt) -- OFF by default, so the default test
# sweep never links libpq and is unaffected by this spec existing.
if(VELOX_BUILD_AUDIT_TIER)
    add_executable(velox_audit_tests
        audit/tailer_test.cpp
        audit/replayer_determinism_test.cpp
        audit/idempotent_ingest_test.cpp
    )
    # PostgreSQL::PostgreSQL is linked directly here (not just via velox_audit, which keeps it
    # PRIVATE) because idempotent_ingest_test.cpp needs libpq itself to load the schema and
    # inspect rows for its fixture -- a test-only exception to "pg_writer.cpp is the only libpq
    # TU", which is about the shipped audit tier, not its test harness.
    target_link_libraries(velox_audit_tests PRIVATE velox_audit PostgreSQL::PostgreSQL
                                                     GTest::gtest_main Threads::Threads)
    target_include_directories(velox_audit_tests PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(velox_audit_tests
        PRIVATE VELOX_REPLAY_DIR="${CMAKE_CURRENT_SOURCE_DIR}/replay"
                VELOX_AUDIT_SQL_INIT="${CMAKE_SOURCE_DIR}/audit/sql/001_init.sql"
    )
    gtest_discover_tests(velox_audit_tests PROPERTIES LABELS "audit")

    # Spawns the real velox_gateway + velox_loadgen + velox_auditd binaries and SIGKILLs
    # Postgres mid-stream -- same "drive the real binary" pattern as recover_sigkill_test.cpp /
    # gateway/e2e_test.cpp.
    add_executable(velox_audit_isolation_test audit/engine_isolation_test.cpp)
    target_link_libraries(velox_audit_isolation_test PRIVATE velox_audit velox_gateway_lib
                                                              velox_marketdata PostgreSQL::PostgreSQL
                                                              GTest::gtest_main Threads::Threads)
    target_include_directories(velox_audit_isolation_test PRIVATE ${CMAKE_SOURCE_DIR})
    target_compile_definitions(velox_audit_isolation_test
        PRIVATE VELOX_AUDIT_SQL_INIT="${CMAKE_SOURCE_DIR}/audit/sql/001_init.sql"
    )
    add_dependencies(velox_audit_isolation_test velox_gateway velox_loadgen velox_auditd)
    target_compile_definitions(velox_audit_isolation_test PRIVATE
        VELOX_GATEWAY_BIN="$<TARGET_FILE:velox_gateway>"
        VELOX_LOADGEN_BIN="$<TARGET_FILE:velox_loadgen>"
        VELOX_AUDITD_BIN="$<TARGET_FILE:velox_auditd>"
    )
    add_test(NAME Audit.EngineIsolation COMMAND velox_audit_isolation_test)
    set_tests_properties(Audit.EngineIsolation PROPERTIES LABELS "audit" TIMEOUT 300)
endif()

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

# Spec 013 T7: velox_adminctl must never write to the filesystem it observes -- grepped, not
# just reviewed. See tests/structural/admin_readonly_test.cpp.
add_executable(velox_admin_readonly_test structural/admin_readonly_test.cpp)
target_link_libraries(velox_admin_readonly_test PRIVATE GTest::gtest_main)
target_compile_definitions(velox_admin_readonly_test
    PRIVATE VELOX_SOURCE_ROOT="${CMAKE_SOURCE_DIR}"
)
add_test(NAME Structural.AdminReadOnly COMMAND velox_admin_readonly_test)
set_tests_properties(Structural.AdminReadOnly PROPERTIES LABELS "admin")
