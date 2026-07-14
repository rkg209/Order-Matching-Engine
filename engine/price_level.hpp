#pragma once

#include "common/types.hpp"
#include "engine/order.hpp"

namespace velox {

// One price level: an intrusive doubly-linked FIFO of orders, all at the same price.
//
// Head is the earliest arrival and therefore the next to fill. Tail is where new orders join.
// That is time priority, and it is the entire reason this is a FIFO and not a heap or a set.
class PriceLevel {
 public:
    PriceLevel() = default;

    void init(Price p) noexcept {
        price_ = p;
        head_ = nullptr;
        tail_ = nullptr;
        totalQty_ = 0;
        count_ = 0;
    }

    // Join the back of the queue. O(1).
    void enqueue(Order* o) noexcept {
        o->prev = tail_;
        o->next = nullptr;
        o->level = this;
        if (tail_ != nullptr) {
            tail_->next = o;
        } else {
            head_ = o;
        }
        tail_ = o;
        totalQty_ += o->remaining;
        ++count_;
    }

    // Remove a specific order. O(1) -- the order carries its own links, so we never search.
    // This is what makes cancel O(1) once the id map has located the order.
    void unlink(Order* o) noexcept {
        if (o->prev != nullptr) {
            o->prev->next = o->next;
        } else {
            head_ = o->next;
        }
        if (o->next != nullptr) {
            o->next->prev = o->prev;
        } else {
            tail_ = o->prev;
        }
        totalQty_ -= o->remaining;
        --count_;
        o->prev = nullptr;
        o->next = nullptr;
        o->level = nullptr;
    }

    // Called when a resting order is partially filled: its remaining quantity shrank, so the
    // level's aggregate must shrink with it. Forgetting this is how quantity conservation
    // silently breaks -- the level would keep advertising liquidity that is no longer there.
    void reduceQuantity(Quantity by) noexcept { totalQty_ -= by; }

    Order* head() const noexcept { return head_; }
    Order* tail() const noexcept { return tail_; }
    bool empty() const noexcept { return head_ == nullptr; }
    Price price() const noexcept { return price_; }
    Quantity totalQuantity() const noexcept { return totalQty_; }
    std::size_t count() const noexcept { return count_; }

 private:
    Order* head_ = nullptr;
    Order* tail_ = nullptr;
    Price price_ = 0;
    Quantity totalQty_ = 0;
    std::size_t count_ = 0;
};

}  // namespace velox
