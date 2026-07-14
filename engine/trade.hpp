#pragma once

#include <cstddef>

#include "common/types.hpp"

namespace velox {

// One executed trade: an aggressor crossing into a resting (passive) order.
struct Trade {
    Seq id;  // deterministic: a monotonic counter, never a UUID or a clock
    OrderId aggressorId;
    OrderId passiveId;

    // The RESTING order's price -- never the aggressor's. A buy at 101 hitting a resting sell
    // at 100 trades at 100; the price improvement accrues to the aggressor. The resting order
    // was there first and advertised the terms it was willing to trade on.
    Price price;

    Quantity quantity;
    Side aggressorSide;
};

// Trades are written into a caller-provided fixed buffer. The matching loop therefore never
// allocates to report its results -- which it would if it returned a std::vector.
struct TradeBuffer {
    Trade* data;
    std::size_t capacity;
    std::size_t count;

    void push(const Trade& t) noexcept {
        if (count < capacity) {
            data[count] = t;
        }
        // Overflow increments count without writing, so the caller can DETECT truncation
        // (count > capacity) rather than silently losing trades. Losing a trade silently is
        // the worst possible failure mode in a matching engine.
        ++count;
    }

    bool overflowed() const noexcept { return count > capacity; }
    void clear() noexcept { count = 0; }
};

}  // namespace velox
