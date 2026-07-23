#include "protocol/decoder.hpp"

#include <cstring>

#include "protocol/wire.hpp"

namespace velox::protocol {

namespace {
bool validSide(std::uint8_t raw) noexcept {
    return raw == 0 || raw == 1;
}
bool validWireOrderType(std::uint8_t raw) noexcept {
    return raw == 0 || raw == 1;
}
bool validWireTif(std::uint8_t raw) noexcept {
    return raw == 0 || raw == 1 || raw == 2;
}
}  // namespace

bool FrameDecoder::feed(const std::byte* data, std::size_t n) noexcept {
    if (terminal_ || len_ + n > kCapacity) {
        return false;
    }
    std::memcpy(buf_ + len_, data, n);
    len_ += n;
    return true;
}

void FrameDecoder::consume(std::size_t n) noexcept {
    std::memmove(buf_, buf_ + n, len_ - n);
    len_ -= n;
}

FrameDecoder::Result FrameDecoder::next(DecodedMessage& out, RejectReason& reason) noexcept {
    if (terminal_) {
        return fail(RejectReason::MalformedFrame, reason);
    }

    // Step 1: need at least the length prefix.
    if (len_ < kFrameHeaderSize) {
        return Result::Incomplete;
    }

    // Step 2: bound-check `length` BEFORE anything else touches it -- the 4 GB-length remote
    // DoS is one line to get wrong, so this happens before the buffer is even consulted for a
    // full frame.
    const std::uint32_t length = wire::getU32(buf_);
    if (length < 1 || length > kMaxFrame) {
        return fail(RejectReason::MalformedFrame, reason);
    }

    // Step 3: wait for the rest of the frame; do not consume anything yet.
    const std::size_t total = kFrameHeaderSize + length;
    if (len_ < total) {
        return Result::Incomplete;
    }

    // Step 4: msgType must be one we know.
    const std::uint8_t rawType = wire::getU8(buf_ + kFrameHeaderSize);
    if (!isKnownMessageType(rawType)) {
        return fail(RejectReason::UnknownMessageType, reason);
    }
    const MessageType type = static_cast<MessageType>(rawType);

    // Step 5: fixed sizes make this exact -- no range check needed.
    if (length != kMsgTypeSize + expectedPayloadSize(type)) {
        return fail(RejectReason::MalformedFrame, reason);
    }

    const std::byte* p = buf_ + kFrameHeaderSize + kMsgTypeSize;

    // Step 6: field-range validation, per type.
    switch (type) {
        case MessageType::Login: {
            LoginMsg m{};
            m.participantId = wire::getI64(p);
            std::memcpy(m.token, p + 8, sizeof(m.token));
            m.clientSeqNum = wire::getU64(p + 8 + 32);
            out.type = type;
            out.login = m;
            break;
        }
        case MessageType::NewOrder: {
            NewOrderMsg m{};
            m.clientSeqNum = wire::getU64(p);
            m.orderId = wire::getI64(p + 8);
            m.instrumentId = wire::getU32(p + 16);
            const std::uint8_t sideRaw = wire::getU8(p + 20);
            const std::uint8_t typeRaw = wire::getU8(p + 21);
            m.price = wire::getI64(p + 22);
            m.quantity = wire::getI64(p + 30);
            const std::uint8_t tifRaw = wire::getU8(p + 38);

            if (!validSide(sideRaw) || !validWireOrderType(typeRaw) || !validWireTif(tifRaw)) {
                return fail(RejectReason::InvalidField, reason);
            }
            m.side = static_cast<Side>(sideRaw);
            m.orderType = static_cast<WireOrderType>(typeRaw);
            m.timeInForce = static_cast<WireTimeInForce>(tifRaw);

            if (m.instrumentId != instrumentId_) {
                return fail(RejectReason::UnknownInstrument, reason);
            }
            if (m.quantity <= 0) {
                return fail(RejectReason::InvalidField, reason);
            }
            if (m.orderType == WireOrderType::Market && m.timeInForce == WireTimeInForce::Fok) {
                return fail(RejectReason::InvalidTifCombination, reason);
            }
            if (m.orderType == WireOrderType::Limit) {
                if (m.price < minPrice_ || m.price > maxPrice_) {
                    return fail(RejectReason::InvalidField, reason);
                }
            } else {
                // MARKET: price ignored, but must still be in range or exactly 0 (plan's table).
                if (m.price != 0 && (m.price < minPrice_ || m.price > maxPrice_)) {
                    return fail(RejectReason::InvalidField, reason);
                }
            }
            out.type = type;
            out.newOrder = m;
            break;
        }
        case MessageType::Cancel: {
            CancelMsg m{};
            m.clientSeqNum = wire::getU64(p);
            m.orderId = wire::getI64(p + 8);
            m.instrumentId = wire::getU32(p + 16);
            if (m.instrumentId != instrumentId_) {
                return fail(RejectReason::UnknownInstrument, reason);
            }
            out.type = type;
            out.cancel = m;
            break;
        }
        case MessageType::CancelReplace: {
            CancelReplaceMsg m{};
            m.clientSeqNum = wire::getU64(p);
            m.orderId = wire::getI64(p + 8);
            m.instrumentId = wire::getU32(p + 16);
            m.newPrice = wire::getI64(p + 20);
            m.newQuantity = wire::getI64(p + 28);
            if (m.instrumentId != instrumentId_) {
                return fail(RejectReason::UnknownInstrument, reason);
            }
            if (m.newQuantity <= 0 || m.newPrice < minPrice_ || m.newPrice > maxPrice_) {
                return fail(RejectReason::InvalidField, reason);
            }
            out.type = type;
            out.cancelReplace = m;
            break;
        }
        case MessageType::Heartbeat: {
            HeartbeatMsg m{};
            m.marker = wire::getU64(p);
            out.type = type;
            out.heartbeat = m;
            break;
        }
        case MessageType::LoginAck: {
            LoginAckMsg m{};
            m.serverSeq = wire::getI64(p);
            m.status = wire::getU8(p + 8);
            out.type = type;
            out.loginAck = m;
            break;
        }
        case MessageType::LoginReject: {
            LoginRejectMsg m{};
            const std::uint8_t r = wire::getU8(p);
            m.reason = static_cast<RejectReason>(r);
            m.reserved = wire::getU8(p + 1);
            out.type = type;
            out.loginReject = m;
            break;
        }
        case MessageType::ExecReport: {
            ExecReportMsg m{};
            m.orderId = wire::getI64(p);
            m.execType = static_cast<ExecType>(wire::getU8(p + 8));
            m.execQty = wire::getI64(p + 9);
            m.leavesQty = wire::getI64(p + 17);
            m.tradePrice = wire::getI64(p + 25);
            m.tradeId = wire::getI64(p + 33);
            m.globalSeq = wire::getI64(p + 41);
            out.type = type;
            out.execReport = m;
            break;
        }
        case MessageType::Reject: {
            RejectMsg m{};
            m.orderId = wire::getI64(p);
            m.reason = static_cast<RejectReason>(wire::getU8(p + 8));
            m.globalSeq = wire::getI64(p + 9);
            out.type = type;
            out.reject = m;
            break;
        }
    }

    // Step 7: only now consume the frame from the reassembly buffer.
    consume(total);
    return Result::Ok;
}

}  // namespace velox::protocol
