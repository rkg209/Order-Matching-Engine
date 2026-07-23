// Spec 007 DoD 3 / NFR-26: hand-picked hostile frames. Every one must produce a structured
// reject and put the decoder into a terminal state -- never a crash, never an attempted resync.

#include <gtest/gtest.h>

#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"
#include "protocol/wire.hpp"

using namespace velox::protocol;

namespace {
constexpr velox::Price kMin = 1 * velox::kPriceScale;
constexpr velox::Price kMax = 10000 * velox::kPriceScale;

// Feeds raw bytes and returns the terminal Invalid's reason, or asserts it never went Invalid.
RejectReason expectInvalid(const std::byte* data, std::size_t n) {
    FrameDecoder decoder(1, kMin, kMax);
    EXPECT_TRUE(decoder.feed(data, n));
    DecodedMessage m;
    RejectReason reason = RejectReason::MalformedFrame;
    FrameDecoder::Result r;
    do {
        r = decoder.next(m, reason);
    } while (r == FrameDecoder::Result::Incomplete &&
             false);  // never loops -- feed is one-shot here
    EXPECT_EQ(r, FrameDecoder::Result::Invalid);
    EXPECT_TRUE(decoder.terminal());
    return reason;
}

std::vector<std::byte> validNewOrder() {
    NewOrderMsg m{};
    m.clientSeqNum = 1;
    m.orderId = 1;
    m.instrumentId = 1;
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Limit;
    m.price = kMin;
    m.quantity = 10;
    m.timeInForce = WireTimeInForce::Day;
    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    return std::vector<std::byte>(buf, buf + n);
}

}  // namespace

TEST(Hostile, LengthAllOnes) {
    std::byte buf[8]{};
    wire::putU32(buf, 0xFFFFFFFFu);
    EXPECT_EQ(expectInvalid(buf, 8), RejectReason::MalformedFrame);
}

TEST(Hostile, LengthZero) {
    std::byte buf[8]{};
    wire::putU32(buf, 0u);
    EXPECT_EQ(expectInvalid(buf, 8), RejectReason::MalformedFrame);
}

TEST(Hostile, LengthShorterThanTypeRequires) {
    auto frame = validNewOrder();
    // Claim a length far smaller than NewOrder's real payload.
    wire::putU32(frame.data(), 5);
    EXPECT_EQ(expectInvalid(frame.data(), frame.size()), RejectReason::MalformedFrame);
}

TEST(Hostile, LengthLongerThanTypeRequires) {
    auto frame = validNewOrder();
    // Claim 10 bytes more than NewOrder actually needs, and supply exactly that many bytes so
    // this is a real "wrong length for this type" mismatch, not merely an incomplete frame.
    const std::uint32_t declaredLength = wire::getU32(frame.data()) + 10;
    wire::putU32(frame.data(), declaredLength);
    frame.resize(frame.size() + 10, std::byte{0});
    EXPECT_EQ(expectInvalid(frame.data(), frame.size()), RejectReason::MalformedFrame);
}

TEST(Hostile, UnknownMessageType) {
    auto frame = validNewOrder();
    wire::putU8(frame.data() + kFrameHeaderSize, 200);
    EXPECT_EQ(expectInvalid(frame.data(), frame.size()), RejectReason::UnknownMessageType);
}

TEST(Hostile, TruncatedMidPayloadStaysIncomplete) {
    auto frame = validNewOrder();
    FrameDecoder decoder(1, kMin, kMax);
    EXPECT_TRUE(decoder.feed(frame.data(), frame.size() - 5));
    DecodedMessage m;
    RejectReason reason;
    EXPECT_EQ(decoder.next(m, reason), FrameDecoder::Result::Incomplete);
    EXPECT_FALSE(decoder.terminal());  // a short read is not hostile, just not done yet
}

TEST(Hostile, QuantityZero) {
    NewOrderMsg m{};
    m.clientSeqNum = 1;
    m.orderId = 1;
    m.instrumentId = 1;
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Limit;
    m.price = kMin;
    m.quantity = 0;
    m.timeInForce = WireTimeInForce::Day;
    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    EXPECT_EQ(expectInvalid(buf, n), RejectReason::InvalidField);
}

TEST(Hostile, QuantityNegative) {
    NewOrderMsg m{};
    m.clientSeqNum = 1;
    m.orderId = 1;
    m.instrumentId = 1;
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Limit;
    m.price = kMin;
    m.quantity = -5;
    m.timeInForce = WireTimeInForce::Day;
    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    EXPECT_EQ(expectInvalid(buf, n), RejectReason::InvalidField);
}

TEST(Hostile, PriceNegative) {
    NewOrderMsg m{};
    m.clientSeqNum = 1;
    m.orderId = 1;
    m.instrumentId = 1;
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Limit;
    m.price = -100;
    m.quantity = 10;
    m.timeInForce = WireTimeInForce::Day;
    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    EXPECT_EQ(expectInvalid(buf, n), RejectReason::InvalidField);
}

TEST(Hostile, SideOutOfRange) {
    auto frame = validNewOrder();
    // side is at payload offset 20 within NewOrder -- header(4) + msgType(1) + clientSeq(8) +
    // orderId(8) + instrumentId(4) = 25.
    frame[25] = std::byte{7};
    EXPECT_EQ(expectInvalid(frame.data(), frame.size()), RejectReason::InvalidField);
}

TEST(Hostile, OrderTypeOutOfRange) {
    auto frame = validNewOrder();
    frame[26] = std::byte{9};  // orderType byte, right after side
    EXPECT_EQ(expectInvalid(frame.data(), frame.size()), RejectReason::InvalidField);
}

TEST(Hostile, MarketPlusFokRejected) {
    NewOrderMsg m{};
    m.clientSeqNum = 1;
    m.orderId = 1;
    m.instrumentId = 1;
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Market;
    m.price = 0;
    m.quantity = 10;
    m.timeInForce = WireTimeInForce::Fok;
    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    EXPECT_EQ(expectInvalid(buf, n), RejectReason::InvalidTifCombination);
}

TEST(Hostile, WrongInstrumentId) {
    NewOrderMsg m{};
    m.clientSeqNum = 1;
    m.orderId = 1;
    m.instrumentId = 42;  // decoder is configured for instrument 1
    m.side = velox::Side::Buy;
    m.orderType = WireOrderType::Limit;
    m.price = kMin;
    m.quantity = 10;
    m.timeInForce = WireTimeInForce::Day;
    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    EXPECT_EQ(expectInvalid(buf, n), RejectReason::UnknownInstrument);
}

TEST(Hostile, FeedOverflowIsRejected) {
    FrameDecoder decoder(1, kMin, kMax);
    std::vector<std::byte> junk(FrameDecoder::kCapacity + 1, std::byte{0x41});
    EXPECT_FALSE(decoder.feed(junk.data(), junk.size()));
}
