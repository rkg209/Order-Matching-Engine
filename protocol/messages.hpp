#pragma once

// One POD per wire message type (Spec 007). These are the decoder's OUTPUT and the encoder's
// INPUT -- never mapped onto the wire bytes directly (no reinterpret_cast, see wire.hpp).

#include <cstdint>

#include "common/types.hpp"
#include "protocol/message_types.hpp"

namespace velox::protocol {

using InstrumentId = std::uint32_t;

struct LoginMsg {
    ParticipantId participantId;
    unsigned char token[32];
    std::uint64_t clientSeqNum;
};

struct NewOrderMsg {
    std::uint64_t clientSeqNum;
    OrderId orderId;
    InstrumentId instrumentId;
    Side side;
    WireOrderType orderType;
    Price price;
    Quantity quantity;
    WireTimeInForce timeInForce;
};

struct CancelMsg {
    std::uint64_t clientSeqNum;
    OrderId orderId;
    InstrumentId instrumentId;
};

struct CancelReplaceMsg {
    std::uint64_t clientSeqNum;
    OrderId orderId;
    InstrumentId instrumentId;
    Price newPrice;
    Quantity newQuantity;
};

struct HeartbeatMsg {
    std::uint64_t marker;
};

struct LoginAckMsg {
    Seq serverSeq;  // globalSeq at the moment of login -- informational only
    std::uint8_t status;
};

struct LoginRejectMsg {
    RejectReason reason;
    std::uint8_t reserved;
};

struct ExecReportMsg {
    OrderId orderId;
    ExecType execType;
    Quantity execQty;
    Quantity leavesQty;
    Price tradePrice;
    std::int64_t tradeId;
    Seq globalSeq;
};

struct RejectMsg {
    OrderId orderId;
    RejectReason reason;
    Seq globalSeq;
};

}  // namespace velox::protocol
