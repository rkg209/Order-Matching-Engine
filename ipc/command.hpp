#pragma once

// The inbound command flyweight (Spec 005).
//
// One POD covering every inbound verb. The ring slot IS the storage: the producer claims a
// slot and writes field values into it in place, so the object never crosses the thread
// boundary -- only its field values do. That is what makes the producer -> consumer handoff
// allocation-free (constitution P1).

#include <type_traits>

#include "common/cache.hpp"
#include "common/types.hpp"
#include "engine/order_book.hpp"

namespace velox::ipc {

enum class CommandKind : std::uint8_t { New = 0, Cancel, Replace };

struct Command {
    OrderId id;     // New/Cancel: the id. Replace: the OLD id.
    OrderId newId;  // Replace only.
    Price price;
    Quantity quantity;
    ParticipantId participant;
    CommandKind kind;
    Side side;
    OrderType type;
};

static_assert(std::is_trivially_copyable_v<Command>, "Command must stay a flat POD");
static_assert(sizeof(Command) <= kCacheLineSize, "Command grew past one cache line");

// Trivial inline accessor: the engine still assigns `seq` itself, so nothing about how a
// Command travels through the ring can make time priority nondeterministic.
inline NewOrder toNewOrder(const Command& c) noexcept {
    return NewOrder{
        .id = c.id,
        .price = c.price,
        .quantity = c.quantity,
        .participant = c.participant,
        .side = c.side,
        .type = c.type,
    };
}

}  // namespace velox::ipc
