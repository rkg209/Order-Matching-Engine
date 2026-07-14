#pragma once

#include <type_traits>

#include "common/types.hpp"

namespace velox {

class PriceLevel;

// A resting or in-flight order.
//
// The intrusive list pointers live INSIDE the order, so the FIFO's nodes ARE the orders.
// There is no separate node type and therefore no node allocation -- which is the whole
// reason not to use std::list here.
struct Order {
    OrderId id;
    Price price;
    Quantity quantity;   // as submitted
    Quantity remaining;  // unfilled
    ParticipantId participant;

    // Time priority is a monotonic COUNTER, not a clock.
    //
    // This is the determinism principle showing up as a data-structure decision. An arrival
    // timestamp would make replay depend on wall-clock, so replaying the same journal on a
    // different day would produce different priority and therefore different trades. Arrival
    // *order* carries exactly the same semantics and is deterministic by construction.
    Seq seq;

    Side side;

    // Intrusive FIFO links within a price level.
    Order* prev;
    Order* next;

    // Back-pointer to the owning level, so a cancel located via the id map can unlink in O(1)
    // without searching for which level the order is in. (Used from Spec 002.)
    PriceLevel* level;
};

// The hot fields must not straddle a cache line. If Order grows past 64 bytes, matching pays
// a second cache line per order touched, on every order -- so this is a hard check, not a
// style preference.
static_assert(std::is_trivially_copyable_v<Order>, "Order must stay a flat POD");
static_assert(sizeof(Order) <= 96, "Order grew; re-check its cache footprint before allowing this");

}  // namespace velox
