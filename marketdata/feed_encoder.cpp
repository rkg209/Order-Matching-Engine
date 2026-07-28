#include "marketdata/feed_encoder.hpp"

#include "protocol/wire.hpp"

namespace velox::marketdata {

namespace {

std::byte* writeHeader(std::byte* dst, protocol::MessageType type,
                       std::size_t payloadSize) noexcept {
    protocol::wire::putU32(dst, static_cast<std::uint32_t>(protocol::kMsgTypeSize + payloadSize));
    protocol::wire::putU8(dst + protocol::kFrameHeaderSize, static_cast<std::uint8_t>(type));
    return dst + protocol::kFrameHeaderSize + protocol::kMsgTypeSize;
}

}  // namespace

std::size_t encodeL2Delta(const L2DeltaMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, protocol::MessageType::L2Delta, protocol::kL2DeltaPayloadSize);
    protocol::wire::putU32(p, m.instrumentId);
    protocol::wire::putU64(p + 4, m.feedSeq);
    protocol::wire::putU8(p + 12, static_cast<std::uint8_t>(m.side));
    protocol::wire::putU8(p + 13, static_cast<std::uint8_t>(m.action));
    protocol::wire::putI64(p + 14, m.price);
    protocol::wire::putI64(p + 22, m.totalQty);
    protocol::wire::putU32(p + 30, m.orderCount);
    return protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kL2DeltaPayloadSize;
}

std::size_t encodeL3Order(const L3OrderMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, protocol::MessageType::L3Order, protocol::kL3OrderPayloadSize);
    protocol::wire::putU32(p, m.instrumentId);
    protocol::wire::putU64(p + 4, m.feedSeq);
    protocol::wire::putU8(p + 12, static_cast<std::uint8_t>(m.action));
    protocol::wire::putI64(p + 13, m.orderId);
    protocol::wire::putU8(p + 21, static_cast<std::uint8_t>(m.side));
    protocol::wire::putI64(p + 22, m.price);
    protocol::wire::putI64(p + 30, m.quantity);
    protocol::wire::putI64(p + 38, m.remaining);
    protocol::wire::putI64(p + 46, m.participant);
    return protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kL3OrderPayloadSize;
}

std::size_t encodeL3Fill(const L3FillMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, protocol::MessageType::L3Fill, protocol::kL3FillPayloadSize);
    protocol::wire::putU32(p, m.instrumentId);
    protocol::wire::putU64(p + 4, m.feedSeq);
    protocol::wire::putI64(p + 12, m.orderId);
    protocol::wire::putI64(p + 20, m.tradeId);
    protocol::wire::putI64(p + 28, m.price);
    protocol::wire::putI64(p + 36, m.quantity);
    protocol::wire::putI64(p + 44, m.remaining);
    return protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kL3FillPayloadSize;
}

std::size_t encodeTradeTick(const TradeTickMsg& m, std::byte* dst) noexcept {
    std::byte* p =
        writeHeader(dst, protocol::MessageType::TradeTick, protocol::kTradeTickPayloadSize);
    protocol::wire::putU32(p, m.instrumentId);
    protocol::wire::putU64(p + 4, m.feedSeq);
    protocol::wire::putI64(p + 12, m.tradeId);
    protocol::wire::putI64(p + 20, m.aggressorId);
    protocol::wire::putI64(p + 28, m.passiveId);
    protocol::wire::putI64(p + 36, m.price);
    protocol::wire::putI64(p + 44, m.quantity);
    protocol::wire::putU8(p + 52, static_cast<std::uint8_t>(m.aggressorSide));
    return protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kTradeTickPayloadSize;
}

std::size_t encodeSnapshotStart(const SnapshotStartMsg& m, std::byte* dst) noexcept {
    std::byte* p =
        writeHeader(dst, protocol::MessageType::SnapshotStart, protocol::kSnapshotStartPayloadSize);
    protocol::wire::putU32(p, m.instrumentId);
    protocol::wire::putU64(p + 4, m.feedSeq);
    protocol::wire::putU64(p + 12, m.restingOrders);
    return protocol::kFrameHeaderSize + protocol::kMsgTypeSize +
           protocol::kSnapshotStartPayloadSize;
}

std::size_t encodeSnapshotEnd(const SnapshotEndMsg& m, std::byte* dst) noexcept {
    std::byte* p =
        writeHeader(dst, protocol::MessageType::SnapshotEnd, protocol::kSnapshotEndPayloadSize);
    protocol::wire::putU32(p, m.instrumentId);
    protocol::wire::putU64(p + 4, m.feedSeq);
    protocol::wire::putU32(p + 12, m.crc32);
    return protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kSnapshotEndPayloadSize;
}

}  // namespace velox::marketdata
