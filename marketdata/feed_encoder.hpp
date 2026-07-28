#pragma once

// FeedEncoder: market-data message struct -> wire bytes (Spec 008 T7). Same shape as
// protocol/encoder.hpp -- explicit little-endian byte stores via protocol::wire, never struct
// punning.

#include <cstddef>

#include "marketdata/feed_messages.hpp"

namespace velox::marketdata {

// Every encode function writes a complete frame ([length][msgType][payload]) into `dst`, which
// must have at least kFrameHeaderSize + kMsgTypeSize + kMaxFrame bytes available (see
// protocol/message_types.hpp), and returns the number of bytes written.
std::size_t encodeL2Delta(const L2DeltaMsg& m, std::byte* dst) noexcept;
std::size_t encodeL3Order(const L3OrderMsg& m, std::byte* dst) noexcept;
std::size_t encodeL3Fill(const L3FillMsg& m, std::byte* dst) noexcept;
std::size_t encodeTradeTick(const TradeTickMsg& m, std::byte* dst) noexcept;
std::size_t encodeSnapshotStart(const SnapshotStartMsg& m, std::byte* dst) noexcept;
std::size_t encodeSnapshotEnd(const SnapshotEndMsg& m, std::byte* dst) noexcept;

}  // namespace velox::marketdata
