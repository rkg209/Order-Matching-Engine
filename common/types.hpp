#pragma once

#include <cstdint>
#include <limits>

namespace velox {

// Prices are scaled integers. There is no floating point anywhere in the engine:
// floats bring rounding error into money and non-associativity into comparison,
// and neither is acceptable in a system whose output must be byte-identical on replay.
using Price = std::int64_t;  // real price * PRICE_SCALE
using Quantity = std::int64_t;
using OrderId = std::int64_t;
using ParticipantId = std::int64_t;
using Seq = std::int64_t;  // arrival order == time priority (see below)

inline constexpr Price kPriceScale = 10000;

enum class Side : std::uint8_t { Buy = 0, Sell = 1 };

inline constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

// Empty-book sentinels.
//
// These specific values are load-bearing, not arbitrary. They make the crossing test
// naturally false on an empty book with no special-case branch:
//
//   a BUY  crosses when  price >= bestAsk  ->  price >= INT64_MAX  is false
//   a SELL crosses when  price <= bestBid  ->  price <= INT64_MIN  is false
//
// The empty-book case falls out of the arithmetic instead of needing a guard, which is
// one fewer branch on the hot path and one fewer place to forget the guard.
inline constexpr Price kBidEmpty = std::numeric_limits<Price>::min();
inline constexpr Price kAskEmpty = std::numeric_limits<Price>::max();

inline constexpr Price emptySentinel(Side s) noexcept {
    return s == Side::Buy ? kBidEmpty : kAskEmpty;
}

}  // namespace velox
