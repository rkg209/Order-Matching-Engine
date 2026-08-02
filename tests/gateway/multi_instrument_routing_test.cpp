// Spec 011 T9.3: multi-instrument gateway routing. Two instruments, two sessions -- an order for
// instrument 2 never appears in shard 1's book; exec reports come back on the originating
// connection for both; an order for an unconfigured instrument is rejected by the decoder and
// closes the connection (the existing hostile-input contract, unchanged by sharding).

#include <gtest/gtest.h>

#include "gateway/auth.hpp"
#include "tests/gateway/gateway_test_harness.hpp"

using namespace velox;
using namespace velox::gateway::test;
using velox::gateway::AuthHandler;

namespace {

AuthHandler authWithTwoUsers() {
    AuthHandler auth;
    AuthHandler::Token ta{};
    ta.fill(0xAA);
    auth.addCredential(1, ta);
    AuthHandler::Token tb{};
    tb.fill(0xBB);
    auth.addCredential(2, tb);
    return auth;
}

}  // namespace

TEST(MultiInstrumentRouting, OrdersRouteToTheirOwnShardOnly) {
    GatewayTestHarness harness("multi_instr", authWithTwoUsers(), {1, 2});

    TestClient clientA(harness.port, /*instrumentId=*/1);
    TestClient clientB(harness.port, /*instrumentId=*/2);
    unsigned char tokA[32];
    makeToken(0xAA, tokA);
    unsigned char tokB[32];
    makeToken(0xBB, tokB);
    ASSERT_TRUE(clientA.login(1, tokA));
    ASSERT_TRUE(clientB.login(2, tokB));

    // A rests on instrument 1; B rests on instrument 2. Neither crosses the other -- different
    // shards, different books, by construction.
    clientA.sendNewOrder(2, 100, Side::Buy, 50 * kPriceScale, 10, /*instrumentId=*/1);
    protocol::DecodedMessage aAck;
    ASSERT_TRUE(clientA.readOne(aAck));
    EXPECT_EQ(aAck.type, protocol::MessageType::ExecReport);
    EXPECT_EQ(aAck.execReport.execType, protocol::ExecType::NewAck);
    EXPECT_EQ(aAck.execReport.orderId, 100);

    clientB.sendNewOrder(2, 200, Side::Buy, 50 * kPriceScale, 10, /*instrumentId=*/2);
    protocol::DecodedMessage bAck;
    ASSERT_TRUE(clientB.readOne(bAck));
    EXPECT_EQ(bAck.type, protocol::MessageType::ExecReport);
    EXPECT_EQ(bAck.execReport.execType, protocol::ExecType::NewAck);
    EXPECT_EQ(bAck.execReport.orderId, 200);

    ASSERT_EQ(harness.shards.size(), 2u);
    const int idx1 = harness.shards.indexOf(1);
    const int idx2 = harness.shards.indexOf(2);
    ASSERT_GE(idx1, 0);
    ASSERT_GE(idx2, 0);

    // Same price on both instruments would cross if they shared a book -- they must not.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (harness.shards[static_cast<std::size_t>(idx1)].matching().processedCount() < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    while (harness.shards[static_cast<std::size_t>(idx2)].matching().processedCount() < 1 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    EXPECT_EQ(harness.shards[static_cast<std::size_t>(idx1)].matching().book().restingOrders(), 1u);
    EXPECT_EQ(harness.shards[static_cast<std::size_t>(idx2)].matching().book().restingOrders(), 1u);
    // Order 200 (instrument 2) never reached shard 1 (instrument 1)'s book -- if it had, shard
    // 1 would show 2 resting orders instead of 1.
    EXPECT_EQ(harness.shards[static_cast<std::size_t>(idx1)].matching().book().bestBid(),
              50 * kPriceScale);
}

// The existing hostile-input contract (tests/gateway/hostile_test.cpp's WrongInstrumentId) still
// holds when the gateway serves more than one instrument: an id outside the configured SET is
// rejected by the decoder, not silently routed anywhere.
TEST(MultiInstrumentRouting, UnconfiguredInstrumentClosesConnection) {
    GatewayTestHarness harness("multi_instr_unknown", authWithTwoUsers(), {1, 2});

    TestClient client(harness.port, /*instrumentId=*/1);
    unsigned char tok[32];
    makeToken(0xAA, tok);
    ASSERT_TRUE(client.login(1, tok));

    client.sendNewOrder(2, 300, Side::Buy, 50 * kPriceScale, 10, /*instrumentId=*/99);
    protocol::DecodedMessage msg;
    // The decoder's UnknownInstrument path is terminal (protocol/decoder.cpp) -- the connection
    // closes without ever producing a reply frame.
    EXPECT_FALSE(client.readOne(msg));
}
