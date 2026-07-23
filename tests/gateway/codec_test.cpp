// Spec 007 DoD 1: round-trip encode -> decode -> identical struct, for every message type,
// plus boundary field values.

#include <gtest/gtest.h>

#include <cstring>

#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"

using namespace velox::protocol;

namespace {

constexpr velox::Price kMin = 1 * velox::kPriceScale;
constexpr velox::Price kMax = 10000 * velox::kPriceScale;

DecodedMessage roundTrip(std::size_t n, const std::byte* buf) {
    FrameDecoder decoder(/*instrumentId=*/1, kMin, kMax);
    RejectReason reason;
    EXPECT_TRUE(decoder.feed(buf, n));
    DecodedMessage out;
    EXPECT_EQ(decoder.next(out, reason), FrameDecoder::Result::Ok);
    return out;
}

}  // namespace

TEST(Codec, Login) {
    LoginMsg m{};
    m.participantId = 42;
    for (int i = 0; i < 32; ++i) m.token[i] = static_cast<unsigned char>(i);
    m.clientSeqNum = 9999999999ULL;  // > 255: proves the 8-byte widening (plan decision 1)

    std::byte buf[64];
    const std::size_t n = encodeLogin(m, buf);
    const DecodedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, MessageType::Login);
    EXPECT_EQ(d.login.participantId, m.participantId);
    EXPECT_EQ(0, std::memcmp(d.login.token, m.token, 32));
    EXPECT_EQ(d.login.clientSeqNum, m.clientSeqNum);
}

TEST(Codec, NewOrderBoundaryValues) {
    NewOrderMsg m{};
    m.clientSeqNum = 1;
    m.orderId = INT64_MAX;
    m.instrumentId = 1;
    m.side = velox::Side::Sell;
    m.orderType = WireOrderType::Limit;
    m.price = kMin;
    m.quantity = INT64_MAX;
    m.timeInForce = WireTimeInForce::Day;

    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    const DecodedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, MessageType::NewOrder);
    EXPECT_EQ(d.newOrder.orderId, INT64_MAX);
    EXPECT_EQ(d.newOrder.price, kMin);
    EXPECT_EQ(d.newOrder.quantity, INT64_MAX);
    EXPECT_EQ(d.newOrder.side, velox::Side::Sell);
}

TEST(Codec, NewOrderAtMaxPrice) {
    NewOrderMsg m{};
    m.clientSeqNum = 2;
    m.orderId = 7;
    m.instrumentId = 1;
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Limit;
    m.price = kMax;
    m.quantity = 1;
    m.timeInForce = WireTimeInForce::Ioc;

    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::NewOrder);
    EXPECT_EQ(d.newOrder.price, kMax);
    EXPECT_EQ(d.newOrder.timeInForce, WireTimeInForce::Ioc);
}

TEST(Codec, MarketOrderZeroPrice) {
    NewOrderMsg m{};
    m.clientSeqNum = 3;
    m.orderId = 8;
    m.instrumentId = 1;
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Market;
    m.price = 0;
    m.quantity = 5;
    m.timeInForce = WireTimeInForce::Day;

    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::NewOrder);
    EXPECT_EQ(d.newOrder.orderType, WireOrderType::Market);
}

TEST(Codec, Cancel) {
    CancelMsg m{1, 55, 1};
    std::byte buf[32];
    const std::size_t n = encodeCancel(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::Cancel);
    EXPECT_EQ(d.cancel.orderId, 55);
}

TEST(Codec, CancelReplace) {
    CancelReplaceMsg m{1, 55, 1, 200 * velox::kPriceScale, 30};
    std::byte buf[48];
    const std::size_t n = encodeCancelReplace(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::CancelReplace);
    EXPECT_EQ(d.cancelReplace.newPrice, 200 * velox::kPriceScale);
    EXPECT_EQ(d.cancelReplace.newQuantity, 30);
}

TEST(Codec, Heartbeat) {
    HeartbeatMsg m{123456789};
    std::byte buf[16];
    const std::size_t n = encodeHeartbeat(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::Heartbeat);
    EXPECT_EQ(d.heartbeat.marker, 123456789u);
}

TEST(Codec, LoginAck) {
    LoginAckMsg m{999, 0};
    std::byte buf[16];
    const std::size_t n = encodeLoginAck(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::LoginAck);
    EXPECT_EQ(d.loginAck.serverSeq, 999);
}

TEST(Codec, LoginReject) {
    LoginRejectMsg m{RejectReason::AuthFailed, 0};
    std::byte buf[16];
    const std::size_t n = encodeLoginReject(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::LoginReject);
    EXPECT_EQ(d.loginReject.reason, RejectReason::AuthFailed);
}

TEST(Codec, ExecReport) {
    ExecReportMsg m{42, ExecType::Fill, 10, 5, 101 * velox::kPriceScale, 7, 12345};
    std::byte buf[64];
    const std::size_t n = encodeExecReport(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::ExecReport);
    EXPECT_EQ(d.execReport.orderId, 42);
    EXPECT_EQ(d.execReport.execType, ExecType::Fill);
    EXPECT_EQ(d.execReport.globalSeq, 12345);
}

TEST(Codec, Reject) {
    RejectMsg m{42, RejectReason::RingFull, 999};
    std::byte buf[32];
    const std::size_t n = encodeReject(m, buf);
    const DecodedMessage d = roundTrip(n, buf);
    ASSERT_EQ(d.type, MessageType::Reject);
    EXPECT_EQ(d.reject.reason, RejectReason::RingFull);
    EXPECT_EQ(d.reject.globalSeq, 999);
}
