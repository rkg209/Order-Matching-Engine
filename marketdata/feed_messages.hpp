#pragma once

// One POD per market-data wire message type (Spec 008 T7). Mirrors protocol/messages.hpp's
// convention exactly: these are feed_decoder's OUTPUT and feed_encoder's INPUT, never mapped
// onto wire bytes directly (no reinterpret_cast -- see protocol/wire.hpp).

#include <cstdint>

#include "common/types.hpp"
#include "protocol/message_types.hpp"
#include "protocol/messages.hpp"

namespace velox::marketdata {

using protocol::InstrumentId;

struct L2DeltaMsg {
    InstrumentId instrumentId;
    std::uint64_t feedSeq;
    Side side;
    protocol::L2Action action;
    Price price;
    Quantity totalQty;
    std::uint32_t orderCount;
};

struct L3OrderMsg {
    InstrumentId instrumentId;
    std::uint64_t feedSeq;
    protocol::L3Action action;
    OrderId orderId;
    Side side;
    Price price;
    Quantity quantity;
    Quantity remaining;
    ParticipantId participant;
};

struct L3FillMsg {
    InstrumentId instrumentId;
    std::uint64_t feedSeq;
    OrderId orderId;
    std::int64_t tradeId;
    Price price;
    Quantity quantity;
    Quantity remaining;
};

struct TradeTickMsg {
    InstrumentId instrumentId;
    std::uint64_t feedSeq;
    std::int64_t tradeId;
    OrderId aggressorId;
    OrderId passiveId;
    Price price;
    Quantity quantity;
    Side aggressorSide;
};

struct SnapshotStartMsg {
    InstrumentId instrumentId;
    std::uint64_t feedSeq;
    std::uint64_t restingOrders;
};

struct SnapshotEndMsg {
    InstrumentId instrumentId;
    std::uint64_t feedSeq;
    std::uint32_t crc32;
};

}  // namespace velox::marketdata
