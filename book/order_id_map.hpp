#pragma once

#include <cstddef>
#include <memory>
#include <type_traits>

#include "common/types.hpp"
#include "engine/order.hpp"

namespace velox::book {

// orderId -> Order*, open-addressed with linear probing.
//
// This is what makes cancel O(1): find the Order here, then unlink it from its level using the
// intrusive pointers it already carries. Without this map, cancelling would mean scanning the
// book for the order, which is O(n) -- and since real order flow is dominated by cancels (often
// well over 90% of messages), an O(n) cancel would dominate everything else the engine does.
//
// Custom rather than std::unordered_map because unordered_map allocates a node per insert and
// its iteration order is unspecified. Both are disqualifying: the first violates the
// zero-allocation rule, the second violates determinism.
//
// Built in Spec 001, exercised from Spec 002 (cancel / cancel-replace).
class OrderIdMap {
 public:
    explicit OrderIdMap(std::size_t capacityPowerOfTwo)
        : capacity_(capacityPowerOfTwo),
          mask_(capacityPowerOfTwo - 1),
          keys_(std::make_unique<OrderId[]>(capacityPowerOfTwo)),
          values_(std::make_unique<Order*[]>(capacityPowerOfTwo)),
          state_(std::make_unique<State[]>(capacityPowerOfTwo)) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            state_[i] = State::Empty;
            values_[i] = nullptr;
        }
    }

    // Returns false if the map is full or the id already exists.
    bool insert(OrderId id, Order* o) noexcept {
        std::size_t i = slot(id);
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            State s = state_[i];
            if (s == State::Occupied && keys_[i] == id) {
                return false;  // duplicate order id
            }
            if (s != State::Occupied) {
                keys_[i] = id;
                values_[i] = o;
                state_[i] = State::Occupied;
                ++size_;
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;  // full
    }

    Order* find(OrderId id) const noexcept {
        std::size_t i = slot(id);
        for (std::size_t probe = 0; probe < capacity_; ++probe) {
            if (state_[i] == State::Empty) {
                return nullptr;  // an empty slot terminates the probe: the id is absent
            }
            if (keys_[i] == id) {
                return values_[i];
            }
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    // Backward-shift deletion. NOT tombstones.
    //
    // This matters far more than it looks. The obvious implementation marks the slot as a
    // TOMBSTONE -- a third state that is "deleted but still terminates nothing", so that keys
    // which probed past it stay reachable. It is correct, and it is a slow-motion disaster:
    // tombstones are never reclaimed, so in a long-running engine where orders continually rest
    // and fill, the table saturates with them. Once it does, every find() probes the ENTIRE
    // table (a tombstone cannot terminate a probe), and the map silently degrades from O(1) to
    // O(capacity).
    //
    // This is not hypothetical. The first version of this file used tombstones, and the
    // benchmark -- cycling millions of orders in and out -- HUNG. Not crashed: hung, which is
    // exactly how this bug would present in production, hours after deploy, under sustained
    // load, with no test failing. See progress_report.md [005].
    //
    // Backward-shift deletion removes the entry and then walks the probe chain forward, pulling
    // back any element that would become unreachable. It leaves the table in the exact state it
    // would have been in had the key never been inserted -- no tombstones, no degradation, no
    // rehash, and still zero allocation.
    bool erase(OrderId id) noexcept {
        std::size_t i = slot(id);
        std::size_t probe = 0;
        for (; probe < capacity_; ++probe) {
            if (state_[i] == State::Empty) {
                return false;  // absent
            }
            if (keys_[i] == id) {
                break;
            }
            i = (i + 1) & mask_;
        }
        if (probe == capacity_) {
            return false;
        }

        // `i` is the hole. Walk forward; any element whose home slot is at or before the hole
        // (cyclically) must be shifted back into it, or it becomes unreachable.
        std::size_t j = i;
        for (;;) {
            j = (j + 1) & mask_;
            if (state_[j] == State::Empty) {
                break;
            }
            const std::size_t home = slot(keys_[j]);

            // Is `home` cyclically outside the open interval (i, j]? If so, element j belongs at
            // or before the hole and must move back to it.
            const bool moveBack = (i <= j) ? (home <= i || home > j) : (home <= i && home > j);
            if (moveBack) {
                keys_[i] = keys_[j];
                values_[i] = values_[j];
                state_[i] = State::Occupied;
                i = j;  // the hole moves to j
            }
        }

        state_[i] = State::Empty;
        values_[i] = nullptr;
        --size_;
        return true;
    }

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return capacity_; }

    // Visit every (OrderId, Order*) occupied slot, in slot order. Const, noexcept as long as
    // `f` is -- this is an introspection accessor for tests and market data (Spec 008 needs
    // level enumeration too), never called from the matching hot path.
    template<class F>
    void forEach(F&& f) const noexcept(std::is_nothrow_invocable_v<F&, OrderId, Order*>) {
        for (std::size_t i = 0; i < capacity_; ++i) {
            if (state_[i] == State::Occupied) {
                f(keys_[i], values_[i]);
            }
        }
    }

 private:
    // Two states only. There is deliberately no Tombstone -- see erase().
    enum class State : std::uint8_t { Empty = 0, Occupied = 1 };

    // Fibonacci hashing. Order ids are frequently sequential, and sequential keys through a
    // mask-based bucket index would cluster badly; multiplying by the golden-ratio constant
    // spreads them. Deterministic -- no random seed, ever (constitution Principle 3).
    std::size_t slot(OrderId id) const noexcept {
        const std::uint64_t h = static_cast<std::uint64_t>(id) * 0x9E3779B97F4A7C15ULL;
        return static_cast<std::size_t>(h >> 32) & mask_;
    }

    std::size_t capacity_;
    std::size_t mask_;
    std::unique_ptr<OrderId[]> keys_;
    std::unique_ptr<Order*[]> values_;
    std::unique_ptr<State[]> state_;
    std::size_t size_ = 0;
};

}  // namespace velox::book
