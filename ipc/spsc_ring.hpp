#pragma once

// Hand-rolled single-producer/single-consumer ring buffer (Spec 005).
//
// Correctness argument (memory ordering), stated explicitly because it IS the correctness
// argument, not decoration:
//   - producer: write the slot, then release-store the write cursor.
//   - consumer: acquire-load the write cursor before reading the slot; after consuming,
//     release-store the read cursor.
//   - each side reads its OWN cursor with relaxed ordering -- only one thread ever writes it.
//
// The cached opposite cursor is the actual win, not just the alignas padding. The producer
// keeps a non-atomic, producer-private `cachedTail_` and only re-reads the consumer's real
// cursor when the cache says the ring looks full; symmetrically for the consumer's
// `cachedHead_`. Without this, every push/pop loads the other side's atomic and the two
// threads ping-pong that cache line forever -- padding alone does not fix that.
//
// Cursors are MONOTONIC uint64_t, masked only at index time -- never wrapped counters. A
// wrapped counter cannot distinguish "full" from "empty"; a monotonic one can, by comparing
// the raw (unmasked) head and tail.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>

#include "common/cache.hpp"

namespace velox::ipc {

template<typename T, std::size_t Capacity = 65536>
class SpscRing {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "SpscRing element must be trivially copyable");

 public:
    using value_type = T;

    SpscRing() : storage_(std::make_unique<T[]>(Capacity)) {
        // Touch every slot at construction so the first claim() does not take a page fault --
        // the same discipline ObjectPool uses for its free-list storage (common/object_pool.hpp).
        for (std::size_t i = 0; i < Capacity; ++i) {
            storage_[i] = T{};
        }
    }

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // --- producer side (single thread only) --------------------------------------------------

    // Returns the next writable slot, or nullptr if the ring is full. The caller writes into
    // the slot in place, then calls publish() -- the slot is never copied twice.
    T* tryClaim() noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        if (head - cachedTail_ >= Capacity) {
            // Cache says full: re-read the consumer's real cursor before giving up.
            cachedTail_ = tail_.value.load(std::memory_order_acquire);
            if (head - cachedTail_ >= Capacity) {
                return nullptr;
            }
        }
        return &storage_[head & (Capacity - 1)];
    }

    // Makes the slot returned by the preceding tryClaim() visible to the consumer.
    void publish() noexcept {
        const std::uint64_t head = head_.value.load(std::memory_order_relaxed);
        head_.value.store(head + 1, std::memory_order_release);
    }

    bool push(const T& v) noexcept {
        T* slot = tryClaim();
        if (slot == nullptr) return false;
        *slot = v;
        publish();
        return true;
    }

    // --- consumer side (single thread only) ---------------------------------------------------

    // Returns the next readable slot, or nullptr if the ring is empty.
    const T* tryPeek() noexcept {
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        if (tail == cachedHead_) {
            cachedHead_ = head_.value.load(std::memory_order_acquire);
            if (tail == cachedHead_) {
                return nullptr;
            }
        }
        return &storage_[tail & (Capacity - 1)];
    }

    // Retires the slot returned by the preceding tryPeek().
    void consume() noexcept {
        const std::uint64_t tail = tail_.value.load(std::memory_order_relaxed);
        tail_.value.store(tail + 1, std::memory_order_release);
    }

    bool pop(T& out) noexcept {
        const T* slot = tryPeek();
        if (slot == nullptr) return false;
        out = *slot;
        consume();
        return true;
    }

    // For tests / diagnostics only -- never on the hot path.
    std::uint64_t headCursor() const noexcept {
        return head_.value.load(std::memory_order_acquire);
    }
    std::uint64_t tailCursor() const noexcept {
        return tail_.value.load(std::memory_order_acquire);
    }

    // Exposed for the false-sharing test only: two alignas(kCacheLineSize) members placed
    // consecutively are guaranteed by the type system to each start on their own cache line and
    // never share one, but the test measures the runtime addresses anyway rather than trusting
    // that the guarantee held (see spsc_ring_test.cpp).
    const void* headCursorAddress() const noexcept { return &head_; }
    const void* tailCursorAddress() const noexcept { return &tail_; }

 private:
    std::unique_ptr<T[]> storage_;

    // Padded so producer and consumer cursors never share a cache line -- verified both at
    // compile time (static_assert below) and at runtime (tests/unit/spsc_ring_test.cpp).
    CachePadded<std::atomic<std::uint64_t>> head_{};  // written only by the producer
    CachePadded<std::atomic<std::uint64_t>> tail_{};  // written only by the consumer

    // Producer-private and consumer-private caches of the OTHER side's cursor. Not atomic:
    // each is touched by exactly one thread. This is what avoids ping-ponging the real atomic
    // cursor's cache line on every single push/pop.
    alignas(kCacheLineSize) std::uint64_t cachedTail_ = 0;
    alignas(kCacheLineSize) std::uint64_t cachedHead_ = 0;
};

}  // namespace velox::ipc
