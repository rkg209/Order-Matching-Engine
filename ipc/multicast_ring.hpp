#pragma once

// Outbound design B (Spec 005 T4): a Disruptor-style multi-consumer ring.
//
// One producer cursor, N independent CachePadded consumer cursors. The producer may claim
// sequence `s` only while `s - min(GATING consumer cursors) < Capacity` -- i.e. it may not
// overwrite a slot the slowest GATING consumer has not yet read. Each consumer reads every event
// and advances only its own cursor; there is no per-event fan-out write, unlike two independent
// SpscRing instances (design A), which costs the engine a second hot-path write per event.
//
// Kept only if the A/B benchmark (velox_ring_bench.cpp) shows it earns its extra complexity;
// see specs/005-spsc-ring-ingress/plan.md for the recorded numbers and decision.
//
// Spec 008 T1: `GatingMask` selects which consumer indices participate in the producer's
// min-cursor gate (bit i set <=> consumer i is gating). A non-gating consumer (e.g. the
// market-data publisher) never stalls the producer -- if it falls a full lap behind, the
// producer simply overwrites the slot it hasn't read yet, and `tryPeekChecked` is how that
// consumer detects it happened (PeekStatus::Lapped) instead of tearing a read against an
// in-flight overwrite.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <type_traits>

#include "common/cache.hpp"

namespace velox::ipc {

template<typename T, std::size_t NumConsumers, std::size_t Capacity = 65536,
         std::uint32_t GatingMask = ~std::uint32_t{0}>
class MulticastRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "MulticastRing element must be trivially copyable");
    static_assert(NumConsumers >= 1, "MulticastRing needs at least one consumer");
    static_assert(NumConsumers <= 32, "GatingMask is a 32-bit mask");

 public:
    enum class PeekStatus : std::uint8_t { Empty, Ok, Lapped };

    MulticastRing() : storage_(std::make_unique<T[]>(Capacity)) {
        for (std::size_t i = 0; i < Capacity; ++i) {
            storage_[i] = T{};
        }
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // --- producer side (single thread only) --------------------------------------------------

    T* tryClaim() noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        if (head - cachedMinTail_ >= Capacity) {
            cachedMinTail_ = minConsumerCursor();
            if (head - cachedMinTail_ >= Capacity) {
                return nullptr;
            }
        }
        return &storage_[head & (Capacity - 1)];
    }

    void publish() noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        head_.value.store(head + 1, std::memory_order_release);
    }

    // --- consumer side (each of the NumConsumers threads calls with its own index) ------------

    // Only meaningful for GATING consumers -- a non-gating consumer that must survive being
    // lapped should use tryPeekChecked() instead, which detects the overwrite rather than
    // silently reading a torn or stale slot.
    const T* tryPeek(std::size_t consumerIdx) noexcept {
        std::uint64_t& cached = cachedHead_[consumerIdx].value;
        const std::uint64_t tail = tails_[consumerIdx].value.load(std::memory_order_relaxed);
        if (tail == cached) {
            cached = head_.value.load(std::memory_order_acquire);
            if (tail == cached) {
                return nullptr;
            }
        }
        return &storage_[tail & (Capacity - 1)];
    }

    void consume(std::size_t consumerIdx) noexcept {
        const std::uint64_t tail = tails_[consumerIdx].value.load(std::memory_order_relaxed);
        tails_[consumerIdx].value.store(tail + 1, std::memory_order_release);
    }

    // Disruptor copy-then-validate, safe against the producer lapping this consumer (which is
    // possible iff bit `consumerIdx` is clear in GatingMask -- a gating consumer can never
    // actually be lapped, since the producer refuses to overwrite a slot it hasn't read).
    //
    //   1. Acquire-load head; if head - tail >= Capacity, the slot this tail points at has
    //      ALREADY been overwritten -- Lapped, no copy attempted.
    //   2. Otherwise copy the slot by value into `out`.
    //   3. Re-load head and re-check. If the producer wrapped around and overwrote the slot
    //      WHILE the copy in step 2 was happening, the copy may have torn -- discard it and
    //      report Lapped rather than hand the caller a mixed-up value.
    //
    // On Lapped, the tail is snapped forward to the head just observed (deliberately dropping to
    // the newest data) so the caller's next call starts from "caught up"; the caller is
    // responsible for resyncing its own state (e.g. rebuilding a mirror from a fresh snapshot).
    PeekStatus tryPeekChecked(std::size_t consumerIdx, T& out) noexcept {
        std::uint64_t& cached = cachedHead_[consumerIdx].value;
        const std::uint64_t tail = tails_[consumerIdx].value.load(std::memory_order_relaxed);
        if (tail == cached) {
            cached = head_.value.load(std::memory_order_acquire);
            if (tail == cached) {
                return PeekStatus::Empty;
            }
        }

        const std::uint64_t head1 = head_.value.load(std::memory_order_acquire);
        if (head1 - tail >= Capacity) {
            return lapse(consumerIdx, head1);
        }

        out = storage_[tail & (Capacity - 1)];

        const std::uint64_t head2 = head_.value.load(std::memory_order_acquire);
        if (head2 - tail >= Capacity) {
            return lapse(consumerIdx, head2);
        }
        return PeekStatus::Ok;
    }

    std::uint64_t lappedCount(std::size_t consumerIdx) const noexcept {
        return lappedCounts_[consumerIdx].value.load(std::memory_order_relaxed);
    }

    // Observability only: how far behind the producer this consumer currently is, in events.
    std::uint64_t lag(std::size_t consumerIdx) const noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_acquire);
        const std::uint64_t tail = tails_[consumerIdx].value.load(std::memory_order_acquire);
        return head - tail;
    }

 private:
    PeekStatus lapse(std::size_t consumerIdx, std::uint64_t newTail) noexcept {
        tails_[consumerIdx].value.store(newTail, std::memory_order_release);
        cachedHead_[consumerIdx].value = newTail;
        lappedCounts_[consumerIdx].value.fetch_add(1, std::memory_order_relaxed);
        return PeekStatus::Lapped;
    }

    std::uint64_t minConsumerCursor() const noexcept {
        std::uint64_t m = std::numeric_limits<std::uint64_t>::max();
        for (std::size_t i = 0; i < NumConsumers; ++i) {
            if ((GatingMask & (std::uint32_t{1} << i)) == 0) {
                continue;  // non-gating: never contributes to the producer's stall condition
            }
            m = std::min(m, tails_[i].value.load(std::memory_order_acquire));
        }
        // GatingMask cleared every bit -- nothing gates the producer at all; that is a valid
        // (if unusual) configuration, so fall back to the producer's own head rather than an
        // uninitialized max().
        return (m == std::numeric_limits<std::uint64_t>::max())
                   ? head_.value.load(std::memory_order_relaxed)
                   : m;
    }

    std::unique_ptr<T[]> storage_;
    CachePadded<std::atomic<std::uint64_t>> head_{};
    CachePadded<std::atomic<std::uint64_t>> tails_[NumConsumers]{};
    CachePadded<std::atomic<std::uint64_t>> lappedCounts_[NumConsumers]{};

    alignas(kCacheLineSize) std::uint64_t cachedMinTail_ = 0;
    // Each consumer thread only ever touches its own slot of this array -- CachePadded so two
    // consumer threads polling concurrently never false-share adjacent cache lines (the plain
    // array this replaced was exactly that bug, unnoticed because there was only ever one real
    // consumer thread until Spec 008 added the market-data publisher as a second).
    CachePadded<std::uint64_t> cachedHead_[NumConsumers]{};
};

}  // namespace velox::ipc
