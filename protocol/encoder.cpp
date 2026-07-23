#include "protocol/encoder.hpp"

#include <cstring>

#include "protocol/wire.hpp"

namespace velox::protocol {

namespace {

// Writes the [length][msgType] header and returns a pointer to where the payload starts.
std::byte* writeHeader(std::byte* dst, MessageType type, std::size_t payloadSize) noexcept {
    wire::putU32(dst, static_cast<std::uint32_t>(kMsgTypeSize + payloadSize));
    wire::putU8(dst + kFrameHeaderSize, static_cast<std::uint8_t>(type));
    return dst + kFrameHeaderSize + kMsgTypeSize;
}

}  // namespace

std::size_t encodeLogin(const LoginMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::Login, kLoginPayloadSize);
    wire::putI64(p, m.participantId);
    std::memcpy(p + 8, m.token, sizeof(m.token));
    wire::putU64(p + 8 + 32, m.clientSeqNum);
    return kFrameHeaderSize + kMsgTypeSize + kLoginPayloadSize;
}

std::size_t encodeNewOrder(const NewOrderMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::NewOrder, kNewOrderPayloadSize);
    wire::putU64(p, m.clientSeqNum);
    wire::putI64(p + 8, m.orderId);
    wire::putU32(p + 16, m.instrumentId);
    wire::putU8(p + 20, static_cast<std::uint8_t>(m.side));
    wire::putU8(p + 21, static_cast<std::uint8_t>(m.orderType));
    wire::putI64(p + 22, m.price);
    wire::putI64(p + 30, m.quantity);
    wire::putU8(p + 38, static_cast<std::uint8_t>(m.timeInForce));
    return kFrameHeaderSize + kMsgTypeSize + kNewOrderPayloadSize;
}

std::size_t encodeCancel(const CancelMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::Cancel, kCancelPayloadSize);
    wire::putU64(p, m.clientSeqNum);
    wire::putI64(p + 8, m.orderId);
    wire::putU32(p + 16, m.instrumentId);
    return kFrameHeaderSize + kMsgTypeSize + kCancelPayloadSize;
}

std::size_t encodeCancelReplace(const CancelReplaceMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::CancelReplace, kCancelReplacePayloadSize);
    wire::putU64(p, m.clientSeqNum);
    wire::putI64(p + 8, m.orderId);
    wire::putU32(p + 16, m.instrumentId);
    wire::putI64(p + 20, m.newPrice);
    wire::putI64(p + 28, m.newQuantity);
    return kFrameHeaderSize + kMsgTypeSize + kCancelReplacePayloadSize;
}

std::size_t encodeHeartbeat(const HeartbeatMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::Heartbeat, kHeartbeatPayloadSize);
    wire::putU64(p, m.marker);
    return kFrameHeaderSize + kMsgTypeSize + kHeartbeatPayloadSize;
}

std::size_t encodeLoginAck(const LoginAckMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::LoginAck, kLoginAckPayloadSize);
    wire::putI64(p, m.serverSeq);
    wire::putU8(p + 8, m.status);
    return kFrameHeaderSize + kMsgTypeSize + kLoginAckPayloadSize;
}

std::size_t encodeLoginReject(const LoginRejectMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::LoginReject, kLoginRejectPayloadSize);
    wire::putU8(p, static_cast<std::uint8_t>(m.reason));
    wire::putU8(p + 1, m.reserved);
    return kFrameHeaderSize + kMsgTypeSize + kLoginRejectPayloadSize;
}

std::size_t encodeExecReport(const ExecReportMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::ExecReport, kExecReportPayloadSize);
    wire::putI64(p, m.orderId);
    wire::putU8(p + 8, static_cast<std::uint8_t>(m.execType));
    wire::putI64(p + 9, m.execQty);
    wire::putI64(p + 17, m.leavesQty);
    wire::putI64(p + 25, m.tradePrice);
    wire::putI64(p + 33, m.tradeId);
    wire::putI64(p + 41, m.globalSeq);
    return kFrameHeaderSize + kMsgTypeSize + kExecReportPayloadSize;
}

std::size_t encodeReject(const RejectMsg& m, std::byte* dst) noexcept {
    std::byte* p = writeHeader(dst, MessageType::Reject, kRejectPayloadSize);
    wire::putI64(p, m.orderId);
    wire::putU8(p + 8, static_cast<std::uint8_t>(m.reason));
    wire::putI64(p + 9, m.globalSeq);
    return kFrameHeaderSize + kMsgTypeSize + kRejectPayloadSize;
}

}  // namespace velox::protocol
