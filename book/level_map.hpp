#pragma once

#include <cstddef>
#include <memory>

#include "common/types.hpp"
#include "engine/price_level.hpp"

namespace velox::book {

// The price levels for ONE side of the book, plus the best price on that side.
//
// STRUCTURE: a direct-indexed array over a bounded price range (root spec section 5:
// "array-indexed when ticks are bounded"). Slot i is the level at price minPrice + i*tick.
//
// Why direct-indexed and not a hash map:
//   - Lookup is an index computation. No hashing, no probing, no collisions. O(1) worst case,
//     not O(1) average.
//   - It is contiguous, so scanning neighbouring prices walks memory linearly and prefetches.
//     That property is what makes the one hard operation here -- recovering the best price
//     after the best level empties -- cheap. A hash map cannot do it at all: it would have to
//     scan every bucket, an O(capacity) hit on a frequent operation, which would land squarely
//     in the p99.
//   - Zero allocation after construction.
//
// The cost is that the price range must be bounded and known up front, and memory is
// proportional to the range rather than to the number of occupied levels. For a single
// instrument with a real tick size that is a few MB, and it buys worst-case O(1) lookup.
//
// A level is "occupied" iff it is non-empty. There is no separate occupancy flag: an empty
// PriceLevel IS an absent price level. That keeps the two from ever disagreeing.
class LevelMap {
 public:
    LevelMap(Side side, Price minPrice, Price maxPrice, Price tick)
        : side_(side),
          minPrice_(minPrice),
          maxPrice_(maxPrice),
          tick_(tick),
          numSlots_(static_cast<std::size_t>((maxPrice - minPrice) / tick) + 1),
          levels_(std::make_unique<PriceLevel[]>(numSlots_)),
          best_(emptySentinel(side)) {
        for (std::size_t i = 0; i < numSlots_; ++i) {
            levels_[i].init(minPrice_ + static_cast<Price>(i) * tick_);
        }
    }

    bool inRange(Price p) const noexcept { return p >= minPrice_ && p <= maxPrice_; }

    // The best price on this side: highest for bids, lowest for asks.
    // Returns the empty sentinel (kBidEmpty / kAskEmpty) when this side is empty, which is what
    // lets the crossing test be branch-free on an empty book. See common/types.hpp.
    Price best() const noexcept { return best_; }
    bool hasBest() const noexcept { return best_ != emptySentinel(side_); }

    PriceLevel* levelAt(Price p) noexcept {
        if (!inRange(p)) {
            return nullptr;
        }
        return &levels_[index(p)];
    }

    const PriceLevel* levelAt(Price p) const noexcept {
        if (!inRange(p)) {
            return nullptr;
        }
        return &levels_[index(p)];
    }

    // Add an order at its price, updating the best price if this order improves it.
    void addOrder(Order* o) noexcept {
        PriceLevel* lvl = &levels_[index(o->price)];
        lvl->enqueue(o);
        if (isBetter(o->price, best_)) {
            best_ = o->price;
        }
    }

    // Called after a level at `p` has become empty. If it was the best, find the next best.
    //
    // This is the one operation in the whole structure that is not O(1), and it is why the
    // contiguous array was chosen: we walk toward worse prices from where the best just was.
    // In a book with any depth the next occupied level is usually adjacent, so this is a
    // handful of cache-hot reads. It degrades only on a book so thin that there is nothing
    // nearby to find -- and on such a book there is also nothing to match against.
    void onLevelEmptied(Price p) noexcept {
        if (p != best_) {
            return;  // an interior level emptied; the best is unaffected
        }
        best_ = nextOccupied(p);
    }

    // The next occupied level strictly beyond `from`, walking toward worse prices (down for
    // bids, up for asks) -- the same directional walk `onLevelEmptied()` performs after the
    // best empties, factored out and made const so the FOK pre-scan (Spec 002) can walk the
    // book without mutating it. Returns the empty sentinel if nothing is found.
    Price nextOccupied(Price from) const noexcept {
        if (side_ == Side::Buy) {
            for (std::size_t i = index(from); i-- > 0;) {
                if (!levels_[i].empty()) {
                    return levels_[i].price();
                }
            }
        } else {
            for (std::size_t i = index(from) + 1; i < numSlots_; ++i) {
                if (!levels_[i].empty()) {
                    return levels_[i].price();
                }
            }
        }
        return emptySentinel(side_);
    }

    Side side() const noexcept { return side_; }
    std::size_t slots() const noexcept { return numSlots_; }

    // Enumeration accessor for tests and market data (Spec 008 needs level enumeration too) --
    // never called from the matching hot path.
    const PriceLevel* levelAtIndex(std::size_t i) const noexcept { return &levels_[i]; }

    // True if price `a` is strictly better than price `b` on this side.
    // Bids: higher is better. Asks: lower is better. Sentinels compare correctly by
    // construction, which is the point of choosing INT64_MIN/INT64_MAX for them.
    bool isBetter(Price a, Price b) const noexcept {
        return side_ == Side::Buy ? (a > b) : (a < b);
    }

 private:
    std::size_t index(Price p) const noexcept {
        return static_cast<std::size_t>((p - minPrice_) / tick_);
    }

    Side side_;
    Price minPrice_;
    Price maxPrice_;
    Price tick_;
    std::size_t numSlots_;
    std::unique_ptr<PriceLevel[]> levels_;  // allocated ONCE, at construction
    Price best_;
};

}  // namespace velox::book
