// Spec 011 T9.4 / T3b: proves the `routes_` race fix under ThreadSanitizer.
//
// Pre-Spec-011, `routes_` was written by the io thread (registerRoute/eraseRoute) and read+
// erased by the router thread (routeExecReport/routeReject) with NO synchronization at all --
// shipped Spec 007 code. Sharding multiplies this by N unless it is actually fixed, so this test
// is what turns "looks right" (gateway/gateway.hpp's ShardCtx::routesMutex) into "proven": many
// sessions crossing orders on two shards concurrently, built with -fsanitize=thread, same pattern
// as tests/unit/spsc_ring_tsan_test.cpp.

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "gateway/auth.hpp"
#include "tests/gateway/gateway_test_harness.hpp"

using namespace velox;
using namespace velox::gateway::test;
using velox::gateway::AuthHandler;

namespace {

AuthHandler authWithFourUsers() {
    AuthHandler auth;
    for (unsigned char p = 1; p <= 4; ++p) {
        AuthHandler::Token t{};
        t.fill(p);
        auth.addCredential(p, t);
    }
    return auth;
}

}  // namespace

TEST(GatewayRoutesRace, ManyOrdersManySessionsAcrossShardsUnderTsan) {
    GatewayTestHarness harness("routes_race_tsan", authWithFourUsers(), {1, 2});

    // Two sessions per instrument, each pair crossing repeatedly -- every crossing order both
    // registers a route (io thread) and, shortly after, has that route looked up and erased by
    // the shard's own router thread (routeExecReport/routeReject) while OTHER sessions on the
    // OTHER shard are doing the exact same thing concurrently on a different routesMutex.
    std::vector<std::unique_ptr<TestClient>> clients;
    for (int i = 0; i < 4; ++i) {
        clients.push_back(std::make_unique<TestClient>(
            harness.port, static_cast<protocol::InstrumentId>(i < 2 ? 1 : 2)));
    }
    for (int i = 0; i < 4; ++i) {
        unsigned char tok[32];
        makeToken(static_cast<unsigned char>(i + 1), tok);
        ASSERT_TRUE(
            clients[static_cast<std::size_t>(i)]->login(static_cast<ParticipantId>(i + 1), tok));
    }

    constexpr int kOrdersPerSession = 100;
    auto driveSession = [&](std::size_t clientIdx, protocol::InstrumentId instrumentId, Side side) {
        TestClient& c = *clients[clientIdx];
        for (int i = 0; i < kOrdersPerSession; ++i) {
            const OrderId id = static_cast<OrderId>(clientIdx * 10000 + i + 1);
            c.sendNewOrder(static_cast<std::uint64_t>(i + 1), id, side, 50 * kPriceScale, 1,
                           instrumentId);
            protocol::DecodedMessage msg;
            // Drain whatever comes back (NewAck, and possibly a Fill for the crossing side) --
            // the point of this test is the concurrent registerRoute/routeExecReport traffic
            // itself, not any particular sequence of replies.
            (void)c.readOne(msg);
        }
    };

    std::vector<std::thread> senders;
    // Instrument 1: client 0 buys, client 1 sells -- crosses every order.
    senders.emplace_back([&] { driveSession(0, 1, Side::Buy); });
    senders.emplace_back([&] { driveSession(1, 1, Side::Sell); });
    // Instrument 2: client 2 buys, client 3 sells -- crosses every order, on the OTHER shard.
    senders.emplace_back([&] { driveSession(2, 2, Side::Buy); });
    senders.emplace_back([&] { driveSession(3, 2, Side::Sell); });

    for (auto& t : senders) {
        t.join();
    }

    // No crash / no TSan report reaching here is the assertion; this line just confirms the
    // harness itself stayed alive throughout.
    SUCCEED();
}
