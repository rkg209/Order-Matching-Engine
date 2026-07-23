// Spec 007 DoD 2: TCP is a byte stream, not a message protocol. A frame split across many
// reads must reassemble to the exact same decoded sequence as one arriving whole -- the bug
// that never shows up in local testing where messages happen to arrive intact.

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"

using namespace velox;
using namespace velox::protocol;

namespace {

constexpr velox::Price kMin = 1 * velox::kPriceScale;
constexpr velox::Price kMax = 10000 * velox::kPriceScale;

std::vector<std::byte> buildStream(int numOrders) {
    std::vector<std::byte> stream;
    for (int i = 0; i < numOrders; ++i) {
        NewOrderMsg m{};
        m.clientSeqNum = static_cast<std::uint64_t>(i + 1);
        m.orderId = i + 1;
        m.instrumentId = 1;
        m.side = (i % 2 == 0) ? velox::Side::Buy : velox::Side::Sell;
        m.orderType = WireOrderType::Limit;
        m.price = kMin + i * 100;
        m.quantity = 10 + i;
        m.timeInForce = WireTimeInForce::Day;
        std::byte buf[64];
        const std::size_t n = encodeNewOrder(m, buf);
        stream.insert(stream.end(), buf, buf + n);
    }
    return stream;
}

// FrameDecoder::kCapacity is small by design (never grows from an attacker-supplied length), so
// a "chunk" here is capped and drained incrementally -- exactly what a real socket read into a
// bounded buffer looks like (see gateway/session.hpp's kReadChunk), rather than one giant feed().
constexpr std::size_t kMaxFeedAtOnce = 64;

std::vector<OrderId> decodeAllFedIn(const std::vector<std::byte>& stream,
                                    const std::vector<std::size_t>& chunkSizes) {
    FrameDecoder decoder(1, kMin, kMax);
    std::vector<OrderId> ids;
    std::size_t offset = 0;
    for (std::size_t requested : chunkSizes) {
        std::size_t remaining = std::min(requested, stream.size() - offset);
        while (remaining > 0) {
            const std::size_t chunk = std::min(remaining, kMaxFeedAtOnce);
            EXPECT_TRUE(decoder.feed(stream.data() + offset, chunk));
            offset += chunk;
            remaining -= chunk;

            DecodedMessage m;
            RejectReason r;
            for (;;) {
                const auto res = decoder.next(m, r);
                if (res == FrameDecoder::Result::Incomplete) break;
                EXPECT_EQ(res, FrameDecoder::Result::Ok);
                if (res != FrameDecoder::Result::Ok) break;
                ids.push_back(m.newOrder.orderId);
            }
        }
    }
    EXPECT_EQ(offset, stream.size());
    return ids;
}

}  // namespace

TEST(Fragmentation, OneBytePerFeed) {
    const auto stream = buildStream(20);
    std::vector<std::size_t> chunks(stream.size(), 1);
    const auto ids = decodeAllFedIn(stream, chunks);
    ASSERT_EQ(ids.size(), 20u);
    for (std::size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], static_cast<OrderId>(i + 1));
}

TEST(Fragmentation, RandomChunkSizes) {
    const auto stream = buildStream(30);
    std::mt19937 rng(12345);
    std::uniform_int_distribution<int> dist(1, 17);
    std::vector<std::size_t> chunks;
    std::size_t total = 0;
    while (total < stream.size()) {
        const std::size_t c = static_cast<std::size_t>(dist(rng));
        chunks.push_back(c);
        total += c;
    }
    const auto ids = decodeAllFedIn(stream, chunks);
    ASSERT_EQ(ids.size(), 30u);
    for (std::size_t i = 0; i < ids.size(); ++i) EXPECT_EQ(ids[i], static_cast<OrderId>(i + 1));
}

TEST(Fragmentation, TwoFramesInOneChunk) {
    const auto stream = buildStream(2);
    const auto ids = decodeAllFedIn(stream, {stream.size()});
    ASSERT_EQ(ids.size(), 2u);
    EXPECT_EQ(ids[0], 1);
    EXPECT_EQ(ids[1], 2);
}

TEST(Fragmentation, FrameSplitAcrossThreeChunks) {
    const auto stream = buildStream(1);
    ASSERT_GE(stream.size(), 3u);
    const std::size_t third = stream.size() / 3;
    const auto ids = decodeAllFedIn(stream, {third, third, stream.size() - 2 * third});
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], 1);
}

TEST(Fragmentation, SameDecodedSequenceRegardlessOfChunking) {
    const auto stream = buildStream(15);
    const auto whole = decodeAllFedIn(stream, {stream.size()});
    const auto byteAtATime = decodeAllFedIn(stream, std::vector<std::size_t>(stream.size(), 1));
    EXPECT_EQ(whole, byteAtATime);
}
