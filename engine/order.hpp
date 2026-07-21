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
    // HOT: touched by matchInto()'s inner loop (engine/order_book.cpp:56-118) on every order it
    // walks, and by unlink() (engine/price_level.hpp:41) on every removal. Grouped first so they
    // land together at the front of the object.
    Quantity remaining;         // unfilled -- read/written every fill
    ParticipantId participant;  // STP check, before every trade
    Price price;                // trade price when this order is the resting/passive side
    OrderId id;                 // written into every Trade this order participates in
    Order* next;                // FIFO walk in matchInto()'s inner while loop
    PriceLevel* level;          // back-pointer so cancel() can unlink without a level lookup
    Side side;                  // resting side, checked by invariants and STP bookkeeping

    // COLD: read only outside the per-fill inner loop -- quantity is for reporting (filled =
    // quantity - remaining), seq only for time-priority comparisons/invariants, prev only when
    // this exact order is being unlinked (once, not per order walked past).
    Quantity quantity;  // as submitted

    // Time priority is a monotonic COUNTER, not a clock.
    //
    // This is the determinism principle showing up as a data-structure decision. An arrival
    // timestamp would make replay depend on wall-clock, so replaying the same journal on a
    // different day would produce different priority and therefore different trades. Arrival
    // *order* carries exactly the same semantics and is deterministic by construction.
    Seq seq;

    Order* prev;  // intrusive FIFO link; only touched by unlink() on the order being removed
};

// The hot fields must not straddle a cache line. If Order grows past 64 bytes, matching pays
// a second cache line per order touched, on every order -- so this is a hard check, not a
// style preference.
static_assert(std::is_trivially_copyable_v<Order>, "Order must stay a flat POD");
static_assert(sizeof(Order) <= 80, "Order grew; re-check its cache footprint before allowing this");

}  // namespace velox
