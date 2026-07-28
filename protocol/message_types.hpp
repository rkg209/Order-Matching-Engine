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

    // Market-data feed (Spec 008, FR-31/32/33). Values 10+ are the free range this file's own
    // comment reserves for exactly this.
    L2Delta = 10,
    L3Order = 11,
    L3Fill = 12,
    TradeTick = 13,
    SnapshotStart = 14,
    SnapshotEnd = 15,
};

// Wire action for an L2Delta message.
enum class L2Action : std::uint8_t { Add = 0, Modify = 1, Delete = 2 };

// Wire action for an L3Order message. Distinct from ipc::UpdateAction (engine-internal) and from
// ExecType (execution-report side) -- this is what a market-data subscriber sees, not what the
// gateway's client sees. `Rejected` is deliberately absent: an order that never rested has
// nothing for an L3 subscriber to reconstruct, so it never reaches the wire at all.
enum class L3Action : std::uint8_t { Rested = 0, Cancelled = 1, Replaced = 2 };

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

// Spec 008 market-data payloads. instrumentId(4)+feedSeq(8) is common to all six.
inline constexpr std::size_t kL2DeltaPayloadSize = 34;        // +side(1)+action(1)+price(8)+
                                                              // totalQty(8)+orderCount(4)
inline constexpr std::size_t kL3OrderPayloadSize = 54;        // +action(1)+orderId(8)+side(1)+
                                                              // price(8)+quantity(8)+remaining(8)+
                                                              // participant(8)
inline constexpr std::size_t kL3FillPayloadSize = 52;         // +orderId(8)+tradeId(8)+price(8)+
                                                              // quantity(8)+remaining(8)
inline constexpr std::size_t kTradeTickPayloadSize = 53;      // +tradeId(8)+aggressorId(8)+
                                                              // passiveId(8)+price(8)+quantity(8)+
                                                              // aggressorSide(1)
inline constexpr std::size_t kSnapshotStartPayloadSize = 20;  // +restingOrders(8)
inline constexpr std::size_t kSnapshotEndPayloadSize = 16;    // +crc32(4)

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
        case MessageType::L2Delta:
            return kL2DeltaPayloadSize;
        case MessageType::L3Order:
            return kL3OrderPayloadSize;
        case MessageType::L3Fill:
            return kL3FillPayloadSize;
        case MessageType::TradeTick:
            return kTradeTickPayloadSize;
        case MessageType::SnapshotStart:
            return kSnapshotStartPayloadSize;
        case MessageType::SnapshotEnd:
            return kSnapshotEndPayloadSize;
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
        case MessageType::L2Delta:
        case MessageType::L3Order:
        case MessageType::L3Fill:
        case MessageType::TradeTick:
        case MessageType::SnapshotStart:
        case MessageType::SnapshotEnd:
            return true;
    }
    return false;
}

}  // namespace velox::protocol
