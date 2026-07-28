// Spec 008 T7: round-trip encode -> decode -> identical struct, for every market-data message
// type. Same pattern as tests/gateway/codec_test.cpp.

#include <gtest/gtest.h>

#include "marketdata/feed_decoder.hpp"
#include "marketdata/feed_encoder.hpp"

using namespace velox;
using namespace velox::marketdata;

namespace {

DecodedFeedMessage roundTrip(std::size_t n, const std::byte* buf) {
    FeedDecoder decoder;
    EXPECT_TRUE(decoder.feed(buf, n));
    DecodedFeedMessage out;
    EXPECT_EQ(decoder.next(out), FeedDecoder::Result::Ok);
    return out;
}

}  // namespace

TEST(MarketDataCodec, L2Delta) {
    L2DeltaMsg m{1, 42, Side::Buy, protocol::L2Action::Modify, 100 * kPriceScale, 500, 3};
    std::byte buf[128];
    const std::size_t n = encodeL2Delta(m, buf);
    const DecodedFeedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, protocol::MessageType::L2Delta);
    EXPECT_EQ(d.l2Delta.instrumentId, m.instrumentId);
    EXPECT_EQ(d.l2Delta.feedSeq, m.feedSeq);
    EXPECT_EQ(d.l2Delta.side, m.side);
    EXPECT_EQ(d.l2Delta.action, m.action);
    EXPECT_EQ(d.l2Delta.price, m.price);
    EXPECT_EQ(d.l2Delta.totalQty, m.totalQty);
    EXPECT_EQ(d.l2Delta.orderCount, m.orderCount);
}

TEST(MarketDataCodec, L3Order) {
    L3OrderMsg m{1, 7, protocol::L3Action::Rested, 99, Side::Sell, 200 * kPriceScale, 10, 4, 55};
    std::byte buf[128];
    const std::size_t n = encodeL3Order(m, buf);
    const DecodedFeedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, protocol::MessageType::L3Order);
    EXPECT_EQ(d.l3Order.orderId, m.orderId);
    EXPECT_EQ(d.l3Order.action, m.action);
    EXPECT_EQ(d.l3Order.side, m.side);
    EXPECT_EQ(d.l3Order.price, m.price);
    EXPECT_EQ(d.l3Order.quantity, m.quantity);
    EXPECT_EQ(d.l3Order.remaining, m.remaining);
    EXPECT_EQ(d.l3Order.participant, m.participant);
}

TEST(MarketDataCodec, L3Fill) {
    L3FillMsg m{1, 8, 99, 5, 200 * kPriceScale, 6, 4};
    std::byte buf[128];
    const std::size_t n = encodeL3Fill(m, buf);
    const DecodedFeedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, protocol::MessageType::L3Fill);
    EXPECT_EQ(d.l3Fill.orderId, m.orderId);
    EXPECT_EQ(d.l3Fill.tradeId, m.tradeId);
    EXPECT_EQ(d.l3Fill.price, m.price);
    EXPECT_EQ(d.l3Fill.quantity, m.quantity);
    EXPECT_EQ(d.l3Fill.remaining, m.remaining);
}

TEST(MarketDataCodec, TradeTick) {
    TradeTickMsg m{1, 9, 5, 10, 11, 200 * kPriceScale, 6, Side::Buy};
    std::byte buf[128];
    const std::size_t n = encodeTradeTick(m, buf);
    const DecodedFeedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, protocol::MessageType::TradeTick);
    EXPECT_EQ(d.tradeTick.tradeId, m.tradeId);
    EXPECT_EQ(d.tradeTick.aggressorId, m.aggressorId);
    EXPECT_EQ(d.tradeTick.passiveId, m.passiveId);
    EXPECT_EQ(d.tradeTick.price, m.price);
    EXPECT_EQ(d.tradeTick.quantity, m.quantity);
    EXPECT_EQ(d.tradeTick.aggressorSide, m.aggressorSide);
}

TEST(MarketDataCodec, SnapshotStart) {
    SnapshotStartMsg m{1, 1, 12345};
    std::byte buf[128];
    const std::size_t n = encodeSnapshotStart(m, buf);
    const DecodedFeedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, protocol::MessageType::SnapshotStart);
    EXPECT_EQ(d.snapshotStart.restingOrders, m.restingOrders);
}

TEST(MarketDataCodec, SnapshotEnd) {
    SnapshotEndMsg m{1, 999, 0xDEADBEEFu};
    std::byte buf[128];
    const std::size_t n = encodeSnapshotEnd(m, buf);
    const DecodedFeedMessage d = roundTrip(n, buf);

    ASSERT_EQ(d.type, protocol::MessageType::SnapshotEnd);
    EXPECT_EQ(d.snapshotEnd.crc32, m.crc32);
}
