#pragma once

// Wire-level enums and fixed frame sizes (Spec 007).
//
// The wire protocol is deliberately its own vocabulary, not a re-export of engine enums --
// `velox::OrderType` has 4 values (Limit/Market/Ioc/Fok) collapsed from two independent wire
// fields (orderType x timeInForce, decision 6 in the plan); reusing it here would hide that
// mapping instead of making it explicit at the one place it happens (decoder.cpp).
//
// velox::Side IS reused as-is: Buy=0/Sell=1 on the wire matches the engine exactly, and a
// parallel wire::Side would be a distinction with no difference.

#include <cstddef>
#include <cstdint>

namespace velox::protocol {

enum class MessageType : std::uint8_t {
    Login = 1,
    NewOrder = 2,
    Cancel = 3,
    CancelReplace = 4,
    Heartbeat = 5,
    LoginAck = 6,
    LoginReject = 7,
    ExecReport = 8,
    Reject = 9,
};

// Wire orderType (distinct from timeInForce -- see plan decision 6).
enum class WireOrderType : std::uint8_t { Limit = 0, Market = 1 };

enum class WireTimeInForce : std::uint8_t { Day = 0, Ioc = 1, Fok = 2 };

enum class ExecType : std::uint8_t {
    NewAck = 0,
    PartialFill = 1,
    Fill = 2,
    Cancelled = 3,
    Replaced = 4,
};

// Coarse and internal-state-free (NFR-26): no offsets, no pointers, no field names, no
// strerror. A hostile client learns only "the frame was bad" or "the field was bad", never why
// in a way that would help it converge on a working exploit.
enum class RejectReason : std::uint8_t {
    MalformedFrame = 0,
    UnknownMessageType = 1,
    InvalidField = 2,
    UnknownInstrument = 3,
    NotAuthenticated = 4,
    DuplicateSeq = 5,
    SequenceGap = 6,
    RingFull = 7,
    EngineReject = 8,
    InvalidTifCombination = 9,
    AuthFailed = 10,
    AlreadyAuthenticated = 11,
};

// [uint32 length][uint8 msgType][payload] -- length counts msgType + payload.
inline constexpr std::size_t kFrameHeaderSize = 4;  // the length prefix itself
inline constexpr std::size_t kMsgTypeSize = 1;
inline constexpr std::size_t kMaxFrame = 64;  // largest payload (LOGIN, decision 1) + slack

// Field sizes only (msgType is not part of "payload" here, matching the spec's table) --
// participantId(8)+token(32)+clientSeqNum(8) = 48 for LOGIN (decision 1: clientSeqNum widened
// from the planning doc's 1 byte to 8, so it does not wrap after 255 messages).
inline constexpr std::size_t kLoginPayloadSize = 48;
inline constexpr std::size_t kNewOrderPayloadSize = 39;
inline constexpr std::size_t kCancelPayloadSize = 20;
inline constexpr std::size_t kCancelReplacePayloadSize = 36;
inline constexpr std::size_t kHeartbeatPayloadSize = 8;
inline constexpr std::size_t kLoginAckPayloadSize = 9;
inline constexpr std::size_t kLoginRejectPayloadSize = 2;
inline constexpr std::size_t kExecReportPayloadSize = 49;
inline constexpr std::size_t kRejectPayloadSize = 17;

// expectedPayloadSize(msgType) returns the size of the payload AFTER msgType, i.e. `length -
// 1`. Fixed per type -- this is what makes decoder validation step 5 ("length !=
// 1 + expectedPayloadSize") exact rather than a range check.
inline constexpr std::size_t expectedPayloadSize(MessageType t) noexcept {
    switch (t) {
        case MessageType::Login:
            return kLoginPayloadSize;
        case MessageType::NewOrder:
            return kNewOrderPayloadSize;
        case MessageType::Cancel:
            return kCancelPayloadSize;
        case MessageType::CancelReplace:
            return kCancelReplacePayloadSize;
        case MessageType::Heartbeat:
            return kHeartbeatPayloadSize;
        case MessageType::LoginAck:
            return kLoginAckPayloadSize;
        case MessageType::LoginReject:
            return kLoginRejectPayloadSize;
        case MessageType::ExecReport:
            return kExecReportPayloadSize;
        case MessageType::Reject:
            return kRejectPayloadSize;
    }
    return 0;
}

inline constexpr bool isKnownMessageType(std::uint8_t raw) noexcept {
    switch (static_cast<MessageType>(raw)) {
        case MessageType::Login:
        case MessageType::NewOrder:
        case MessageType::Cancel:
        case MessageType::CancelReplace:
        case MessageType::Heartbeat:
        case MessageType::LoginAck:
        case MessageType::LoginReject:
        case MessageType::ExecReport:
        case MessageType::Reject:
            return true;
    }
    return false;
}

}  // namespace velox::protocol
