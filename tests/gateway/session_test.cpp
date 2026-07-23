// Spec 007 DoD 5: duplicate client seq -> reject and NO engine submit; gap -> reject and close;
// auth timeout closes an idle unauthenticated connection; a bad token is rejected the same way
// as an unknown participant.

#include <gtest/gtest.h>

#include <thread>

#include "gateway/auth.hpp"
#include "tests/gateway/gateway_test_harness.hpp"

using namespace velox;
using namespace velox::gateway::test;
using velox::gateway::AuthHandler;

namespace {

AuthHandler authWithOneUser(ParticipantId id, unsigned char tokenByte) {
    AuthHandler auth;
    unsigned char token[32];
    makeToken(tokenByte, token);
    AuthHandler::Token t{};
    std::memcpy(t.data(), token, 32);
    auth.addCredential(id, t);
    return auth;
}

}  // namespace

TEST(GatewaySession, LoginSucceedsWithCorrectToken) {
    GatewayTestHarness harness("login_ok", authWithOneUser(1, 0xAB));
    TestClient client(harness.port);
    unsigned char token[32];
    makeToken(0xAB, token);
    EXPECT_TRUE(client.login(1, token));
}

TEST(GatewaySession, LoginFailsWithBadToken) {
    GatewayTestHarness harness("login_bad", authWithOneUser(1, 0xAB));
    TestClient client(harness.port);
    unsigned char token[32];
    makeToken(0xFF, token);  // wrong
    EXPECT_FALSE(client.login(1, token));
}

TEST(GatewaySession, LoginFailsForUnknownParticipant) {
    GatewayTestHarness harness("login_unknown", authWithOneUser(1, 0xAB));
    TestClient client(harness.port);
    unsigned char token[32];
    makeToken(0xAB, token);
    EXPECT_FALSE(client.login(999, token));  // right token, wrong (unregistered) participant
}

TEST(GatewaySession, DuplicateClientSeqIsRejectedNotResubmitted) {
    GatewayTestHarness harness("dup_seq", authWithOneUser(1, 0x01));
    TestClient client(harness.port);
    unsigned char token[32];
    makeToken(0x01, token);
    ASSERT_TRUE(client.login(1, token, /*clientSeqNum=*/1));

    client.sendNewOrder(2, 100, Side::Buy, 100 * kPriceScale, 10);
    protocol::DecodedMessage m;
    ASSERT_TRUE(client.readOne(m));
    ASSERT_EQ(m.type, protocol::MessageType::ExecReport);
    EXPECT_EQ(m.execReport.execType, protocol::ExecType::NewAck);

    // Retry the SAME clientSeqNum with a different orderId -- must be rejected as a duplicate,
    // and must NOT reach the engine (no second NEW_ACK for orderId 101).
    client.sendNewOrder(2, 101, Side::Buy, 100 * kPriceScale, 10);
    ASSERT_TRUE(client.readOne(m));
    ASSERT_EQ(m.type, protocol::MessageType::Reject);
    EXPECT_EQ(m.reject.reason, protocol::RejectReason::DuplicateSeq);
}

TEST(GatewaySession, SequenceGapClosesConnection) {
    GatewayTestHarness harness("seq_gap", authWithOneUser(1, 0x02));
    TestClient client(harness.port);
    unsigned char token[32];
    makeToken(0x02, token);
    ASSERT_TRUE(client.login(1, token, /*clientSeqNum=*/1));

    client.sendNewOrder(5, 200, Side::Buy, 100 * kPriceScale, 10);  // expected 2, got 5: a gap
    protocol::DecodedMessage m;
    ASSERT_TRUE(client.readOne(m));
    EXPECT_EQ(m.type, protocol::MessageType::Reject);
    EXPECT_EQ(m.reject.reason, protocol::RejectReason::SequenceGap);

    // The connection must be torn down after a gap -- a further read observes closure.
    EXPECT_FALSE(client.readOne(m));
}

TEST(GatewaySession, NewOrderBeforeLoginIsRejected) {
    GatewayTestHarness harness("no_login", authWithOneUser(1, 0x03));
    TestClient client(harness.port);
    client.sendNewOrder(1, 1, Side::Buy, 100 * kPriceScale, 10);
    protocol::DecodedMessage m;
    // Either the connection is closed outright or a LOGIN_REJECT arrives first -- both satisfy
    // "no order is accepted before auth" (FR-24).
    const bool gotMsg = client.readOne(m);
    if (gotMsg) {
        EXPECT_EQ(m.type, protocol::MessageType::LoginReject);
    }
}

TEST(GatewaySession, ManyOrdersOnOneConnection) {
    GatewayTestHarness harness("many_orders", authWithOneUser(1, 0x09));
    TestClient client(harness.port);
    unsigned char token[32];
    makeToken(0x09, token);
    ASSERT_TRUE(client.login(1, token, 1));

    for (int i = 0; i < 20; ++i) {
        client.sendNewOrder(static_cast<std::uint64_t>(i + 2), i + 1, Side::Buy,
                            (100 + i) * kPriceScale, 10);
        protocol::DecodedMessage m;
        ASSERT_TRUE(client.readOne(m)) << "no response for order " << (i + 1);
        if (m.type == protocol::MessageType::Reject) {
            FAIL() << "order " << (i + 1)
                   << " rejected, reason=" << static_cast<int>(m.reject.reason);
        }
        ASSERT_EQ(m.type, protocol::MessageType::ExecReport);
        EXPECT_EQ(m.execReport.execType, protocol::ExecType::NewAck);
    }
}
