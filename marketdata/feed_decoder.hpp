#pragma once

// FeedDecoder: subscriber-side stateful TCP-stream reassembly + validation for the market-data
// feed (Spec 008 T7). Same shape as protocol/decoder.hpp: a small FIXED reassembly buffer that
// never grows from an attacker/peer-supplied length, terminal-on-invalid, no resync attempted
// after a bad frame.

#include <cstddef>

#include "marketdata/feed_messages.hpp"
#include "protocol/message_types.hpp"

namespace velox::marketdata {

struct DecodedFeedMessage {
    protocol::MessageType type;
    L2DeltaMsg l2Delta;
    L3OrderMsg l3Order;
    L3FillMsg l3Fill;
    TradeTickMsg tradeTick;
    SnapshotStartMsg snapshotStart;
    SnapshotEndMsg snapshotEnd;
};

class FeedDecoder {
 public:
    // Comfortably larger than the widest real MD frame (4 + 1 + 54 = 59 B for L3_ORDER), fixed
    // for the lifetime of the decoder.
    static constexpr std::size_t kCapacity = 256;

    enum class Result { Ok, Incomplete, Invalid };

    bool feed(const std::byte* data, std::size_t n) noexcept;
    Result next(DecodedFeedMessage& out) noexcept;
    bool terminal() const noexcept { return terminal_; }

 private:
    Result fail() noexcept {
        terminal_ = true;
        return Result::Invalid;
    }

    void consume(std::size_t n) noexcept;

    std::byte buf_[kCapacity];
    std::size_t len_ = 0;
    bool terminal_ = false;
};

}  // namespace velox::marketdata
