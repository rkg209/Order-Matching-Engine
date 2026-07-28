#include "marketdata/feed_decoder.hpp"

#include <cstring>

#include "protocol/wire.hpp"

namespace velox::marketdata {

bool FeedDecoder::feed(const std::byte* data, std::size_t n) noexcept {
    if (terminal_ || len_ + n > kCapacity) {
        return false;
    }
    std::memcpy(buf_ + len_, data, n);
    len_ += n;
    return true;
}

void FeedDecoder::consume(std::size_t n) noexcept {
    std::memmove(buf_, buf_ + n, len_ - n);
    len_ -= n;
}

FeedDecoder::Result FeedDecoder::next(DecodedFeedMessage& out) noexcept {
    if (terminal_) {
        return fail();
    }
    if (len_ < protocol::kFrameHeaderSize) {
        return Result::Incomplete;
    }

    const std::uint32_t length = protocol::wire::getU32(buf_);
    if (length < 1 || length > protocol::kMaxFrame) {
        return fail();
    }

    const std::size_t total = protocol::kFrameHeaderSize + length;
    if (len_ < total) {
        return Result::Incomplete;
    }

    const std::uint8_t rawType = protocol::wire::getU8(buf_ + protocol::kFrameHeaderSize);
    if (!protocol::isKnownMessageType(rawType)) {
        return fail();
    }
    const protocol::MessageType type = static_cast<protocol::MessageType>(rawType);
    if (length != protocol::kMsgTypeSize + protocol::expectedPayloadSize(type)) {
        return fail();
    }

    const std::byte* p = buf_ + protocol::kFrameHeaderSize + protocol::kMsgTypeSize;

    switch (type) {
        case protocol::MessageType::L2Delta: {
            L2DeltaMsg m{};
            m.instrumentId = protocol::wire::getU32(p);
            m.feedSeq = protocol::wire::getU64(p + 4);
            m.side = static_cast<Side>(protocol::wire::getU8(p + 12));
            m.action = static_cast<protocol::L2Action>(protocol::wire::getU8(p + 13));
            m.price = protocol::wire::getI64(p + 14);
            m.totalQty = protocol::wire::getI64(p + 22);
            m.orderCount = protocol::wire::getU32(p + 30);
            out.type = type;
            out.l2Delta = m;
            break;
        }
        case protocol::MessageType::L3Order: {
            L3OrderMsg m{};
            m.instrumentId = protocol::wire::getU32(p);
            m.feedSeq = protocol::wire::getU64(p + 4);
            m.action = static_cast<protocol::L3Action>(protocol::wire::getU8(p + 12));
            m.orderId = protocol::wire::getI64(p + 13);
            m.side = static_cast<Side>(protocol::wire::getU8(p + 21));
            m.price = protocol::wire::getI64(p + 22);
            m.quantity = protocol::wire::getI64(p + 30);
            m.remaining = protocol::wire::getI64(p + 38);
            m.participant = protocol::wire::getI64(p + 46);
            out.type = type;
            out.l3Order = m;
            break;
        }
        case protocol::MessageType::L3Fill: {
            L3FillMsg m{};
            m.instrumentId = protocol::wire::getU32(p);
            m.feedSeq = protocol::wire::getU64(p + 4);
            m.orderId = protocol::wire::getI64(p + 12);
            m.tradeId = protocol::wire::getI64(p + 20);
            m.price = protocol::wire::getI64(p + 28);
            m.quantity = protocol::wire::getI64(p + 36);
            m.remaining = protocol::wire::getI64(p + 44);
            out.type = type;
            out.l3Fill = m;
            break;
        }
        case protocol::MessageType::TradeTick: {
            TradeTickMsg m{};
            m.instrumentId = protocol::wire::getU32(p);
            m.feedSeq = protocol::wire::getU64(p + 4);
            m.tradeId = protocol::wire::getI64(p + 12);
            m.aggressorId = protocol::wire::getI64(p + 20);
            m.passiveId = protocol::wire::getI64(p + 28);
            m.price = protocol::wire::getI64(p + 36);
            m.quantity = protocol::wire::getI64(p + 44);
            m.aggressorSide = static_cast<Side>(protocol::wire::getU8(p + 52));
            out.type = type;
            out.tradeTick = m;
            break;
        }
        case protocol::MessageType::SnapshotStart: {
            SnapshotStartMsg m{};
            m.instrumentId = protocol::wire::getU32(p);
            m.feedSeq = protocol::wire::getU64(p + 4);
            m.restingOrders = protocol::wire::getU64(p + 12);
            out.type = type;
            out.snapshotStart = m;
            break;
        }
        case protocol::MessageType::SnapshotEnd: {
            SnapshotEndMsg m{};
            m.instrumentId = protocol::wire::getU32(p);
            m.feedSeq = protocol::wire::getU64(p + 4);
            m.crc32 = protocol::wire::getU32(p + 12);
            out.type = type;
            out.snapshotEnd = m;
            break;
        }
        default:
            // Order-entry message types never appear on this feed's decoder.
            return fail();
    }

    consume(total);
    return Result::Ok;
}

}  // namespace velox::marketdata
