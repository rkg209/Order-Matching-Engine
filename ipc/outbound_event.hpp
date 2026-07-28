#pragma once

// The outbound event flyweight (Spec 005).
//
// Carries either a Trade or an order-status change out of the matching thread, tagged by kind.
// Kept <= 64 bytes; Spec 007/008 will widen the status side of this, so the tag enum is left
// open rather than assumed to be exactly two values forever.

#include <cstdint>
#include <type_traits>

#include "common/types.hpp"
#include "engine/order_book.hpp"
#include "engine/trade.hpp"

namespace velox::ipc {

enum class OutboundKind : std::uint8_t { TradeEvent = 0, StatusEvent, OrderUpdate };

// What changed about a resting order, for the market-data L3/L2 path (Spec 008). Distinct from
// SubmitStatus: SubmitStatus is the wire-facing outcome of one command, UpdateAction is the
// book-state transition a market-data subscriber needs to replay to keep its mirror in sync.
enum class UpdateAction : std::uint8_t { Rested, Cancelled, Rejected, Replaced };

struct StatusChange {
    OrderId orderId;
    SubmitStatus status;
};

// Exactly 48 bytes, no padding -- six int64_t, matched to what
// marketdata::mirror_digest needs to reproduce a sequencer::OrderRecord bit-for-bit (Spec 008).
struct OrderUpdate {
    OrderId orderId;
    Price price;
    Quantity quantity;   // as originally submitted
    Quantity remaining;  // unfilled at the end of the command that produced this event
    ParticipantId participant;
    Seq seq;  // the book's own arrival seq -- needed for digest equality with the live book
};

static_assert(sizeof(OrderUpdate) == 48, "OrderUpdate must fill the payload exactly");

// The payloads are mutually exclusive (selected by `kind`), so they are overlaid rather than
// laid out side by side -- that is what keeps this at one cache line as Trade grows.
union OutboundPayload {
    Trade trade;                // valid when kind == TradeEvent
    StatusChange statusChange;  // valid when kind == StatusEvent
    OrderUpdate orderUpdate;    // valid when kind == OrderUpdate

    OutboundPayload() noexcept : trade{} {}
};

struct OutboundEvent {
    // Spec 008: kind/side/status/action live in the byte range that was previously 7 bytes of
    // pure padding after a lone `kind` field -- OutboundPayload is still exactly 48 bytes, so
    // OutboundEvent is unchanged at 64. `side`/`status`/`action` are populated only for the
    // OutboundKind that uses them; the others leave them at their zero-initialized default.
    OutboundKind kind;
    Side side;
    SubmitStatus status;
    UpdateAction action;
    std::uint32_t reserved;
    OutboundPayload payload;
    Seq globalSeq;  // Spec 007: stamped by MatchingThread::dispatch(), ring-arrival order --
                    // never wall-clock, so it stays deterministic (constitution P4).
};

static_assert(std::is_trivially_copyable_v<OutboundEvent>, "OutboundEvent must stay a flat POD");
static_assert(sizeof(OutboundEvent) == 64, "OutboundEvent must occupy exactly one cache line");

inline OutboundEvent tradeEvent(const Trade& t, Seq globalSeq) noexcept {
    OutboundEvent e{};
    e.kind = OutboundKind::TradeEvent;
    e.payload.trade = t;
    e.globalSeq = globalSeq;
    return e;
}

inline OutboundEvent statusEvent(OrderId id, SubmitStatus st, Seq globalSeq) noexcept {
    OutboundEvent e{};
    e.kind = OutboundKind::StatusEvent;
    e.payload.statusChange = StatusChange{id, st};
    e.globalSeq = globalSeq;
    return e;
}

inline OutboundEvent orderUpdateEvent(UpdateAction action, Side side, const OrderUpdate& u,
                                      Seq globalSeq,
                                      SubmitStatus status = SubmitStatus::Ok) noexcept {
    OutboundEvent e{};
    e.kind = OutboundKind::OrderUpdate;
    e.side = side;
    e.action = action;
    e.status = status;  // meaningful only when action == Rejected -- the reason it was rejected
    e.payload.orderUpdate = u;
    e.globalSeq = globalSeq;
    return e;
}

}  // namespace velox::ipc
