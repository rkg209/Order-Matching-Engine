#pragma once

#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
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
//   - Zero allocation after construction.
//
// The cost is that the price range must be bounded and known up front, and memory is
// proportional to the range rather than to the number of occupied levels. For a single
// instrument with a real tick size that is a few MB, and it buys worst-case O(1) lookup.
//
// A level is "occupied" iff it is non-empty. There is no separate occupancy flag on PriceLevel
// itself: an empty PriceLevel IS an absent price level, and PriceLevel::empty() remains the
// single source of truth for that. What follows is a two-level bitset SUMMARY of that truth,
// so that recovering the best price after the best level empties -- the one operation here that
// is not O(1) -- does not have to walk the raw level array to do it.
//
//   l0_: one bit per price slot. Bit i set <=> levels_[i] is non-empty.
//   l1_: one bit per l0_ WORD. Bit w set <=> l0_[w] has any bit set (i.e. some slot in that
//        64-slot range is occupied). A third level is not needed at realistic sizes: even a
//        million-slot book needs only ~15625 l0 words, i.e. ~245 l1 words -- still a handful of
//        cache lines, and nextOccupied() below never has to touch more than that.
//
// Both are allocated once, at construction, alongside levels_ (NFR-11: pre-sized, never grows).
// The only mutation points are addOrder()'s empty->non-empty transition and onLevelEmptied()'s
// non-empty->empty transition -- see their comments below.
class LevelMap {
 public:
    LevelMap(Side side, Price minPrice, Price maxPrice, Price tick)
        : side_(side),
          minPrice_(minPrice),
          maxPrice_(maxPrice),
          tick_(tick),
          numSlots_(static_cast<std::size_t>((maxPrice - minPrice) / tick) + 1),
          levels_(std::make_unique<PriceLevel[]>(numSlots_)),
          l0Words_((numSlots_ + kBitsPerWord - 1) / kBitsPerWord),
          l1Words_((l0Words_ + kBitsPerWord - 1) / kBitsPerWord),
          l0_(std::make_unique<std::uint64_t[]>(l0Words_)),
          l1_(std::make_unique<std::uint64_t[]>(l1Words_)),
          best_(emptySentinel(side)) {
        for (std::size_t i = 0; i < numSlots_; ++i) {
            levels_[i].init(minPrice_ + static_cast<Price>(i) * tick_);
        }
        for (std::size_t i = 0; i < l0Words_; ++i) l0_[i] = 0;
        for (std::size_t i = 0; i < l1Words_; ++i) l1_[i] = 0;
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
        const std::size_t i = index(o->price);
        PriceLevel* lvl = &levels_[i];
        const bool wasEmpty = lvl->empty();
        lvl->enqueue(o);
        if (wasEmpty) {
            setOccupied(i);
        }
        if (isBetter(o->price, best_)) {
            best_ = o->price;
        }
    }

    // Called after a level at `p` has become empty -- ALWAYS exactly then, never otherwise; see
    // the call sites in engine/order_book.cpp (each is already gated on `level->empty()`), which
    // is what makes this the one and only non-empty->empty mutation point for the bitset.
    //
    // Clearing the bit is unconditional. Recovering the best price is not: if an interior level
    // (not the best) emptied, the tracked best is still correct and there is nothing to do.
    void onLevelEmptied(Price p) noexcept {
        clearOccupied(index(p));
        if (p != best_) {
            return;  // an interior level emptied; the best is unaffected
        }
        best_ = nextOccupied(p);
    }

    // The next occupied level strictly beyond `from`, walking toward worse prices (down for
    // bids, up for asks) -- the same directional walk `onLevelEmptied()` performs after the
    // best empties, factored out and made const so the FOK pre-scan (Spec 002) can walk the
    // book without mutating it. Returns the empty sentinel if nothing is found.
    //
    // Backed by the l0_/l1_ occupancy bitset: a couple of ctz/clz instructions per level of the
    // hierarchy, rather than a linear scan of the (possibly ~20k-slot) level array. That linear
    // scan was the one real tail-latency defect in this structure on a thin/gapped book -- see
    // Spec 004.
    Price nextOccupied(Price from) const noexcept {
        const std::size_t idx = index(from);
        const std::size_t found =
            (side_ == Side::Buy) ? findSetBitBelow(idx) : findSetBitAbove(idx);
        if (found == kNotFound) {
            return emptySentinel(side_);
        }
        return levels_[found].price();
    }

    Side side() const noexcept { return side_; }
    std::size_t slots() const noexcept { return numSlots_; }

    // Enumeration accessor for tests and market data (Spec 008 needs level enumeration too) --
    // never called from the matching hot path.
    const PriceLevel* levelAtIndex(std::size_t i) const noexcept { return &levels_[i]; }

    // Introspection accessor for the invariant checker (Spec 003/004): does the occupancy
    // bitset consider slot `i` occupied? Never called from the matching hot path.
    bool occupiedBit(std::size_t i) const noexcept {
        return (l0_[i / kBitsPerWord] & (std::uint64_t{1} << (i % kBitsPerWord))) != 0;
    }

    // True if price `a` is strictly better than price `b` on this side.
    // Bids: higher is better. Asks: lower is better. Sentinels compare correctly by
    // construction, which is the point of choosing INT64_MIN/INT64_MAX for them.
    bool isBetter(Price a, Price b) const noexcept {
        return side_ == Side::Buy ? (a > b) : (a < b);
    }

 private:
    static constexpr std::size_t kBitsPerWord = 64;
    static constexpr std::size_t kNotFound = std::numeric_limits<std::size_t>::max();

    std::size_t index(Price p) const noexcept {
        return static_cast<std::size_t>((p - minPrice_) / tick_);
    }

    // Set slot `i` occupied. Only called on an empty->non-empty transition (addOrder() already
    // checks `wasEmpty` before calling), so the l0 bit is always 0 -> 1 here; the l1 bit is
    // updated too, since a previously-all-empty l0 word just gained its first set bit.
    void setOccupied(std::size_t i) noexcept {
        const std::size_t w = i / kBitsPerWord;
        l0_[w] |= (std::uint64_t{1} << (i % kBitsPerWord));
        l1_[w / kBitsPerWord] |= (std::uint64_t{1} << (w % kBitsPerWord));
#ifndef NDEBUG
        assert(!levels_[i].empty());
#endif
    }

    // Clear slot `i`. Called unconditionally from onLevelEmptied(), which is itself only ever
    // called right after the level in question became empty -- see that function's comment.
    void clearOccupied(std::size_t i) noexcept {
        const std::size_t w = i / kBitsPerWord;
        l0_[w] &= ~(std::uint64_t{1} << (i % kBitsPerWord));
        if (l0_[w] == 0) {
            l1_[w / kBitsPerWord] &= ~(std::uint64_t{1} << (w % kBitsPerWord));
        }
#ifndef NDEBUG
        assert(levels_[i].empty());
#endif
    }

    // Highest set bit strictly less than `idx`, or kNotFound. Used by the BUY side (worse means
    // lower price, i.e. a lower slot index).
    std::size_t findSetBitBelow(std::size_t idx) const noexcept {
        const std::size_t w = idx / kBitsPerWord;
        const std::size_t bitInWord = idx % kBitsPerWord;

        const std::uint64_t mask =
            (bitInWord == 0) ? std::uint64_t{0} : ((std::uint64_t{1} << bitInWord) - 1);
        const std::uint64_t masked = l0_[w] & mask;
        if (masked != 0) {
            return w * kBitsPerWord + (kBitsPerWord - 1 - std::countl_zero(masked));
        }
        if (w == 0) {
            return kNotFound;
        }

        std::size_t curL1 = (w - 1) / kBitsPerWord;
        const std::size_t l1Bit = (w - 1) % kBitsPerWord;
        std::uint64_t l1mask = (l1Bit == kBitsPerWord - 1)
                                   ? ~std::uint64_t{0}
                                   : ((std::uint64_t{1} << (l1Bit + 1)) - 1);
        for (;;) {
            const std::uint64_t l1word = l1_[curL1] & l1mask;
            if (l1word != 0) {
                const std::size_t prevL0 =
                    curL1 * kBitsPerWord + (kBitsPerWord - 1 - std::countl_zero(l1word));
                const std::uint64_t l0word = l0_[prevL0];
                return prevL0 * kBitsPerWord + (kBitsPerWord - 1 - std::countl_zero(l0word));
            }
            if (curL1 == 0) {
                return kNotFound;
            }
            --curL1;
            l1mask = ~std::uint64_t{0};  // subsequent (lower) l1 words are checked in full
        }
    }

    // Lowest set bit strictly greater than `idx`, or kNotFound. Used by the SELL side (worse
    // means higher price, i.e. a higher slot index).
    std::size_t findSetBitAbove(std::size_t idx) const noexcept {
        const std::size_t w = idx / kBitsPerWord;
        const std::size_t bitInWord = idx % kBitsPerWord;

        // Keeps bits strictly greater than bitInWord (clears bits [0, bitInWord]). Guarded at
        // bitInWord==63 to avoid a shift-by-64, which would be a 1ULL<<(bitInWord+1) otherwise.
        const std::uint64_t mask = (bitInWord == kBitsPerWord - 1)
                                       ? std::uint64_t{0}
                                       : ~((std::uint64_t{2} << bitInWord) - 1);
        const std::uint64_t masked = l0_[w] & mask;
        if (masked != 0) {
            return w * kBitsPerWord + std::countr_zero(masked);
        }

        const std::size_t firstNextWord = w + 1;
        if (firstNextWord >= l0Words_) {
            return kNotFound;
        }
        std::size_t curL1 = firstNextWord / kBitsPerWord;
        const std::size_t l1Bit = firstNextWord % kBitsPerWord;
        std::uint64_t l1mask = ~((std::uint64_t{1} << l1Bit) - 1);
        for (; curL1 < l1Words_; ++curL1) {
            const std::uint64_t l1word = l1_[curL1] & l1mask;
            if (l1word != 0) {
                const std::size_t nextL0 = curL1 * kBitsPerWord + std::countr_zero(l1word);
                if (nextL0 >= l0Words_) {
                    return kNotFound;
                }
                const std::uint64_t l0word = l0_[nextL0];
                return nextL0 * kBitsPerWord + std::countr_zero(l0word);
            }
            l1mask = ~std::uint64_t{0};  // subsequent (higher) l1 words are checked in full
        }
        return kNotFound;
    }

    Side side_;
    Price minPrice_;
    Price maxPrice_;
    Price tick_;
    std::size_t numSlots_;
    std::unique_ptr<PriceLevel[]> levels_;  // allocated ONCE, at construction

    std::size_t l0Words_;
    std::size_t l1Words_;
    std::unique_ptr<std::uint64_t[]> l0_;  // allocated ONCE, at construction
    std::unique_ptr<std::uint64_t[]> l1_;  // allocated ONCE, at construction

    Price best_;
};

}  // namespace velox::book
