#pragma once

// Outbound design B (Spec 005 T4): a Disruptor-style multi-consumer ring.
//
// One producer cursor, N independent CachePadded consumer cursors. The producer may claim
// sequence `s` only while `s - min(consumer cursors) < Capacity` -- i.e. it may not overwrite a
// slot the slowest consumer has not yet read. Each consumer reads every event and advances only
// its own cursor; there is no per-event fan-out write, unlike two independent SpscRing
// instances (design A), which costs the engine a second hot-path write per event.
//
// Kept only if the A/B benchmark (velox_ring_bench.cpp) shows it earns its extra complexity;
// see specs/005-spsc-ring-ingress/plan.md for the recorded numbers and decision.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "common/cache.hpp"

namespace velox::ipc {

template<typename T, std::size_t NumConsumers, std::size_t Capacity = 65536>
class MulticastRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>,
                  "MulticastRing element must be trivially copyable");
    static_assert(NumConsumers >= 1, "MulticastRing needs at least one consumer");

 public:
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

    const T* tryPeek(std::size_t consumerIdx) noexcept {
        std::uint64_t& cached = cachedHead_[consumerIdx];
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

 private:
    std::uint64_t minConsumerCursor() const noexcept {
        std::uint64_t m = tails_[0].value.load(std::memory_order_acquire);
        for (std::size_t i = 1; i < NumConsumers; ++i) {
            m = std::min(m, tails_[i].value.load(std::memory_order_acquire));
        }
        return m;
    }

    std::unique_ptr<T[]> storage_;
    CachePadded<std::atomic<std::uint64_t>> head_{};
    CachePadded<std::atomic<std::uint64_t>> tails_[NumConsumers]{};

    alignas(kCacheLineSize) std::uint64_t cachedMinTail_ = 0;
    // Each consumer thread only ever touches its own slot of this array.
    std::uint64_t cachedHead_[NumConsumers] = {};
};

}  // namespace velox::ipc
