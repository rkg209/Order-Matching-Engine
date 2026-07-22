#pragma once

// The outbound event flyweight (Spec 005).
//
// Carries either a Trade or an order-status change out of the matching thread, tagged by kind.
// Kept <= 64 bytes; Spec 007/008 will widen the status side of this, so the tag enum is left
// open rather than assumed to be exactly two values forever.

#include <cstdint>
#include <type_traits>

#include "common/cache.hpp"
#include "common/types.hpp"
#include "engine/order_book.hpp"
#include "engine/trade.hpp"

namespace velox::ipc {

enum class OutboundKind : std::uint8_t { TradeEvent = 0, StatusEvent };

struct StatusChange {
    OrderId orderId;
    SubmitStatus status;
};

// The two payloads are mutually exclusive (selected by `kind`), so they are overlaid rather
// than laid out side by side -- that is what keeps this at one cache line as Trade grows.
union OutboundPayload {
    Trade trade;                // valid when kind == TradeEvent
    StatusChange statusChange;  // valid when kind == StatusEvent

    OutboundPayload() noexcept : trade{} {}
};

struct OutboundEvent {
    OutboundKind kind;
    OutboundPayload payload;
};

static_assert(std::is_trivially_copyable_v<OutboundEvent>, "OutboundEvent must stay a flat POD");
static_assert(sizeof(OutboundEvent) <= kCacheLineSize, "OutboundEvent grew past one cache line");

inline OutboundEvent tradeEvent(const Trade& t) noexcept {
    OutboundEvent e{};
    e.kind = OutboundKind::TradeEvent;
    e.payload.trade = t;
    return e;
}

inline OutboundEvent statusEvent(OrderId id, SubmitStatus st) noexcept {
    OutboundEvent e{};
    e.kind = OutboundKind::StatusEvent;
    e.payload.statusChange = StatusChange{id, st};
    return e;
}

}  // namespace velox::ipc
