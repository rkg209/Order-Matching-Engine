// Spec 007 DoD 7: execution reports route back to the ORIGINATING connection. Two sessions, A's
// aggressor crosses B's resting order -- each must see exactly its own EXEC_REPORT, never the
// other's.

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

TEST(GatewayRouting, ExecReportsGoOnlyToOwningSession) {
    GatewayTestHarness harness("routing", authWithTwoUsers());

    TestClient clientA(harness.port);
    TestClient clientB(harness.port);
    unsigned char tokA[32];
    makeToken(0xAA, tokA);
    unsigned char tokB[32];
    makeToken(0xBB, tokB);
    ASSERT_TRUE(clientA.login(1, tokA));
    ASSERT_TRUE(clientB.login(2, tokB));

    // B rests a SELL at 100; A's BUY at 100 crosses it.
    clientB.sendNewOrder(2, 1000, Side::Sell, 100 * kPriceScale, 10);
    protocol::DecodedMessage bAck;
    ASSERT_TRUE(clientB.readOne(bAck));
    ASSERT_EQ(bAck.type, protocol::MessageType::ExecReport);
    EXPECT_EQ(bAck.execReport.orderId, 1000);
    EXPECT_EQ(bAck.execReport.execType, protocol::ExecType::NewAck);

    clientA.sendNewOrder(2, 2000, Side::Buy, 100 * kPriceScale, 10);
    protocol::DecodedMessage aAck;
    ASSERT_TRUE(clientA.readOne(aAck));
    ASSERT_EQ(aAck.type, protocol::MessageType::ExecReport);
    EXPECT_EQ(aAck.execReport.orderId, 2000);
    EXPECT_EQ(aAck.execReport.execType, protocol::ExecType::NewAck);

    // Both sides now see a FILL for their OWN order id, never the other's.
    protocol::DecodedMessage aFill;
    ASSERT_TRUE(clientA.readOne(aFill));
    ASSERT_EQ(aFill.type, protocol::MessageType::ExecReport);
    EXPECT_EQ(aFill.execReport.orderId, 2000);
    EXPECT_EQ(aFill.execReport.execType, protocol::ExecType::Fill);

    protocol::DecodedMessage bFill;
    ASSERT_TRUE(clientB.readOne(bFill));
    ASSERT_EQ(bFill.type, protocol::MessageType::ExecReport);
    EXPECT_EQ(bFill.execReport.orderId, 1000);
    EXPECT_EQ(bFill.execReport.execType, protocol::ExecType::Fill);

    // Same globalSeq (the trade's stamped sequence) on both sides' fill reports.
    EXPECT_EQ(aFill.execReport.globalSeq, bFill.execReport.globalSeq);
}
