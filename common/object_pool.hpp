#pragma once

#include <cstddef>
#include <memory>
#include <new>

namespace velox {

// A pre-allocated, fixed-capacity object pool.
//
// This is the mechanism that makes "zero allocation on the hot path" true BY CONSTRUCTION
// rather than by discipline. The distinction matters: hunting allocations with a profiler
// finds the ones you wrote today; making allocation structurally impossible prevents the
// ones you would have written tomorrow.
//
// The storage is one contiguous block allocated ONCE at construction (startup). That is a
// permitted allocation -- the constitution forbids allocation *per operation* at steady
// state, not the existence of memory. acquire() and release() never allocate.
//
// Contiguity is not incidental: the free list threads through the block itself, so pooled
// objects are neighbours in memory. The intrusive order lists that chase these pointers are
// therefore usually chasing into cache. A linked list is normally a latency sin; it is
// tolerable here only because of this.
template<typename T>
class ObjectPool {
 public:
    explicit ObjectPool(std::size_t capacity)
        : storage_(std::make_unique<Slot[]>(capacity)), capacity_(capacity) {
        // Thread the free list through the slots, and TOUCH every page while doing it, so the
        // first acquire() in the hot path does not take a page fault. A page fault mid-match is
        // a syscall you cannot see in the mean and cannot miss in the p999.
        for (std::size_t i = 0; i + 1 < capacity_; ++i) {
            storage_[i].next = &storage_[i + 1];
        }
        if (capacity_ > 0) {
            storage_[capacity_ - 1].next = nullptr;
            free_ = &storage_[0];
        }
    }

    // Returns nullptr when exhausted. It does NOT fall back to allocating.
    //
    // A fallback allocation would be a hidden latency cliff that fires exactly when the system
    // is under maximum load -- i.e. at the worst possible moment. The caller must turn a null
    // into backpressure (NFR-10). A bounded rejection beats an unbounded stall.
    T* acquire() noexcept {
        if (free_ == nullptr) {
            return nullptr;
        }
        Slot* s = free_;
        free_ = s->next;
        ++inUse_;
        return reinterpret_cast<T*>(&s->value);
    }

    void release(T* p) noexcept {
        if (p == nullptr) {
            return;
        }
        Slot* s = reinterpret_cast<Slot*>(p);
        s->next = free_;
        free_ = s;
        --inUse_;
    }

    std::size_t capacity() const noexcept { return capacity_; }
    std::size_t inUse() const noexcept { return inUse_; }
    std::size_t available() const noexcept { return capacity_ - inUse_; }

 private:
    // The free-list pointer overlays the object storage: a slot is either a live T or a link
    // in the free list, never both. This is why the pool costs no per-object bookkeeping.
    union Slot {
        Slot* next;
        alignas(T) unsigned char value[sizeof(T)];

        Slot() : next(nullptr) {}
        ~Slot() {}
    };

    std::unique_ptr<Slot[]> storage_;
    Slot* free_ = nullptr;
    std::size_t capacity_ = 0;
    std::size_t inUse_ = 0;
};

}  // namespace velox
