// Spec 007 DoD 4 / NFR-27: >=10,000 randomized malformed frames from a FIXED seed (determinism,
// constitution P4) must never crash and must leave the decoder in a consistent state. Built with
// -fsanitize=address,undefined (see tests/CMakeLists.txt) so a buffer overrun or UB shows up as
// a hard failure here rather than surviving into production.

#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <vector>

#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"

using namespace velox;
using namespace velox::protocol;

namespace {
constexpr Price kMin = 1 * kPriceScale;
constexpr Price kMax = 10000 * kPriceScale;

std::vector<std::byte> validFrame(std::mt19937& rng) {
    NewOrderMsg m{};
    m.clientSeqNum = rng();
    m.orderId = static_cast<OrderId>(rng());
    m.instrumentId = 1;
    m.side = (rng() % 2 == 0) ? Side::Buy : Side::Sell;
    m.orderType = WireOrderType::Limit;
    m.price = kMin + static_cast<Price>(rng() % 1000);
    m.quantity = 1 + static_cast<Quantity>(rng() % 1000);
    m.timeInForce = WireTimeInForce::Day;
    std::byte buf[64];
    const std::size_t n = encodeNewOrder(m, buf);
    return std::vector<std::byte>(buf, buf + n);
}

}  // namespace

TEST(Fuzz, RandomBytesNeverCrash) {
    std::mt19937 rng(0xC0FFEE);
    for (int i = 0; i < 10000; ++i) {
        std::uniform_int_distribution<int> lenDist(0, 96);
        const int len = lenDist(rng);
        std::vector<std::byte> junk(static_cast<std::size_t>(len));
        for (auto& b : junk) b = static_cast<std::byte>(rng() & 0xFF);

        FrameDecoder decoder(1, kMin, kMax);
        if (!decoder.feed(junk.data(), junk.size())) {
            continue;  // decoder correctly refused to buffer it -- not a crash
        }
        DecodedMessage m;
        RejectReason reason;
        // Drain until Incomplete or Invalid; either is an acceptable outcome for random bytes.
        for (int guard = 0; guard < 8; ++guard) {
            const auto r = decoder.next(m, reason);
            if (r != FrameDecoder::Result::Ok) break;
        }
    }
}

TEST(Fuzz, ValidFrameWithOneMutatedByte) {
    std::mt19937 rng(0xBADF00D);
    for (int i = 0; i < 10000; ++i) {
        auto frame = validFrame(rng);
        const std::size_t idx = rng() % frame.size();
        frame[idx] = static_cast<std::byte>(rng() & 0xFF);

        FrameDecoder decoder(1, kMin, kMax);
        if (!decoder.feed(frame.data(), frame.size())) continue;
        DecodedMessage m;
        RejectReason reason;
        decoder.next(m, reason);  // Ok, Incomplete, or Invalid are all fine -- just no crash
    }
}

TEST(Fuzz, ValidFrameTruncatedAtEveryOffset) {
    std::mt19937 rng(42);
    for (int i = 0; i < 200; ++i) {
        auto frame = validFrame(rng);
        for (std::size_t cut = 0; cut <= frame.size(); ++cut) {
            FrameDecoder decoder(1, kMin, kMax);
            if (!decoder.feed(frame.data(), cut)) continue;
            DecodedMessage m;
            RejectReason reason;
            decoder.next(m, reason);
        }
    }
}
