#pragma once

// The subscriber-side book (Spec 008 T5). Built ENTIRELY from the OutboundEvent stream (Trade +
// OrderUpdate) -- never from the engine's own OrderBook/Order/PriceLevel types, which hold raw
// pointers meaningless outside the matching thread. This is deliberately off the hot path: it
// allocates freely (std::map, std::deque, std::unordered_map), the same allowance CLAUDE.md
// gives every module outside engine/ and book/.
//
// FR-34's oracle (marketdata/mirror_digest.hpp) needs this class to reproduce the exact canonical
// walk recovery::computeDigest() uses: bids best->worst, asks best->worst, FIFO within a level.
// std::map is ordered by key, so bids_ uses std::greater<Price> (best bid = highest price first)
// and asks_ uses the default std::less<Price> (best ask = lowest price first) -- both walk
// best-to-worst by iterating forward. std::deque preserves arrival (FIFO) order on push_back,
// exactly matching PriceLevel::enqueue()'s tail-append.

#include <cstdint>
#include <deque>
#include <map>
#include <unordered_map>

#include "common/types.hpp"
#include "engine/trade.hpp"
#include "ipc/outbound_event.hpp"

namespace velox::marketdata {

struct MirrorOrder {
    OrderId id;
    Price price;
    Quantity quantity;   // as originally submitted
    Quantity remaining;  // unfilled right now
    ParticipantId participant;
    Seq seq;
};

class BookMirror {
 public:
    void clear() noexcept {
        bids_.clear();
        asks_.clear();
        index_.clear();
        lastSeq_ = 0;
        tradeCount_ = 0;
    }

    // Dispatches on OutboundKind. StatusEvent carries no L3 information and is ignored --
    // TradeEvent/OrderUpdate together are a complete description of every book mutation
    // (including self-trade-prevention passive-cancels, which ride an OrderUpdate{Cancelled}
    // exactly like a real cancel -- see ipc::OutboundEvent's Spec 008 doc comment).
    void apply(const ipc::OutboundEvent& e) {
        switch (e.kind) {
            case ipc::OutboundKind::TradeEvent:
                applyTrade(e.payload.trade);
                return;
            case ipc::OutboundKind::OrderUpdate:
                applyOrderUpdate(e.action, e.side, e.payload.orderUpdate);
                return;
            case ipc::OutboundKind::StatusEvent:
                return;
        }
    }

    void applyTrade(const Trade& t) noexcept {
        // Only the PASSIVE side is resting at the moment a trade prints -- the aggressor's
        // residual (if it rests at all) arrives as its own, separate OrderUpdate{Rested}
        // afterward. Mirrors engine/order_book.cpp's matchInto(): reduce `remaining`, keep the
        // order in place (FIFO priority is not lost on a partial fill), erase it if it hit zero.
        reduce(t.passiveId, t.quantity);
        ++tradeCount_;
    }

    void applyOrderUpdate(ipc::UpdateAction action, Side side, const ipc::OrderUpdate& u) {
        if (u.seq > lastSeq_) {
            lastSeq_ = u.seq;
        }
        switch (action) {
            case ipc::UpdateAction::Rested:
                insert(side, u);
                return;
            case ipc::UpdateAction::Cancelled:
            case ipc::UpdateAction::Replaced:
                // Both tag a genuinely resting order's removal (a real cancel, an STP-passive
                // victim, or Replace's OLD id) -- erase() it.
                erase(u.orderId);
                return;
            case ipc::UpdateAction::Rejected:
                // This id was NEVER inserted (the order never rested at all -- rejected outright,
                // or fully filled with nothing left to rest). MUST be a true no-op: with a small
                // id pool, this id can coincide with a completely different order that IS
                // genuinely resting right now, and erasing it here would silently desync the
                // mirror from a book mutation that never happened (see matching_thread.hpp's
                // emitOrderUpdate for the full story -- this was a real bug, caught by
                // tests/marketdata/reconstruction_test.cpp).
                return;
        }
    }

    Seq lastSeq() const noexcept { return lastSeq_; }
    Seq tradeCount() const noexcept { return tradeCount_; }
    std::size_t restingOrders() const noexcept { return index_.size(); }

    Quantity levelTotal(Side side, Price price) const noexcept {
        if (side == Side::Buy) {
            return levelTotalIn(bids_, price);
        }
        return levelTotalIn(asks_, price);
    }

    std::uint32_t levelOrderCount(Side side, Price price) const noexcept {
        if (side == Side::Buy) {
            return levelOrderCountIn(bids_, price);
        }
        return levelOrderCountIn(asks_, price);
    }

    // 0 if `id` is not (or no longer) resting -- e.g. a trade that fully filled it, which is
    // exactly when a caller (Publisher's L3Fill encoding) needs to report `remaining == 0`.
    Quantity remainingOf(OrderId id) const noexcept {
        const auto it = index_.find(id);
        if (it == index_.end()) {
            return 0;
        }
        const Location& loc = it->second;
        if (loc.side == Side::Buy) {
            return remainingIn(bids_, loc.price, id);
        }
        return remainingIn(asks_, loc.price, id);
    }

    // Snapshot-load path only (marketdata::Publisher's resync): the resting-order walk that
    // rebuilds bids_/asks_/index_ carries no trade history, so lastSeq/tradeCount must be
    // handed in directly from the engine's own counters (OrderBook::lastSeq()/tradeCount()).
    void seedCounters(Seq lastSeq, Seq tradeCount) noexcept {
        lastSeq_ = lastSeq;
        tradeCount_ = tradeCount;
    }

    // Canonical walk: bids best->worst then asks best->worst, FIFO within a level -- the exact
    // order mirror_digest.hpp (and recovery::computeDigest) need. `f(Side, const MirrorOrder&)`.
    template<class F>
    void forEachOrder(F&& f) const {
        for (const auto& [price, orders] : bids_) {
            for (const MirrorOrder& o : orders) {
                f(Side::Buy, o);
            }
        }
        for (const auto& [price, orders] : asks_) {
            for (const MirrorOrder& o : orders) {
                f(Side::Sell, o);
            }
        }
    }

    // Spec 010 T1: the per-level sibling of forEachOrder -- a visualizer's ladder wants one row
    // per price level (aggregate qty + order count), not one callback per resting order. Same
    // canonical walk (bids best->worst then asks best->worst) so a caller that wants top-N just
    // takes the first N callbacks per side. Folds levelTotal/levelOrderCount's logic inline while
    // already iterating the level's deque, rather than calling back into levelTotalIn() per level
    // (which would re-walk each level's orders a second time -- O(n^2) for a full-book walk).
    // `f(Side, Price, Quantity total, std::uint32_t orders)`.
    template<class F>
    void forEachLevel(F&& f) const {
        for (const auto& [price, orders] : bids_) {
            Quantity total = 0;
            for (const MirrorOrder& o : orders) {
                total += o.remaining;
            }
            f(Side::Buy, price, total, static_cast<std::uint32_t>(orders.size()));
        }
        for (const auto& [price, orders] : asks_) {
            Quantity total = 0;
            for (const MirrorOrder& o : orders) {
                total += o.remaining;
            }
            f(Side::Sell, price, total, static_cast<std::uint32_t>(orders.size()));
        }
    }

 private:
    using Levels = std::map<Price, std::deque<MirrorOrder>>;
    using BidLevels = std::map<Price, std::deque<MirrorOrder>, std::greater<Price>>;

    template<class LevelsT>
    static Quantity levelTotalIn(const LevelsT& levels, Price price) noexcept {
        const auto it = levels.find(price);
        if (it == levels.end()) {
            return 0;
        }
        Quantity total = 0;
        for (const MirrorOrder& o : it->second) {
            total += o.remaining;
        }
        return total;
    }

    template<class LevelsT>
    static std::uint32_t levelOrderCountIn(const LevelsT& levels, Price price) noexcept {
        const auto it = levels.find(price);
        return it == levels.end() ? 0 : static_cast<std::uint32_t>(it->second.size());
    }

    template<class LevelsT>
    static Quantity remainingIn(const LevelsT& levels, Price price, OrderId id) noexcept {
        const auto it = levels.find(price);
        if (it == levels.end()) {
            return 0;
        }
        for (const MirrorOrder& o : it->second) {
            if (o.id == id) {
                return o.remaining;
            }
        }
        return 0;
    }

    struct Location {
        Side side;
        Price price;
    };

    void insert(Side side, const ipc::OrderUpdate& u) {
        const MirrorOrder o{u.orderId, u.price, u.quantity, u.remaining, u.participant, u.seq};
        if (side == Side::Buy) {
            bids_[u.price].push_back(o);
        } else {
            asks_[u.price].push_back(o);
        }
        index_[u.orderId] = Location{side, u.price};
    }

    void erase(OrderId id) {
        const auto it = index_.find(id);
        if (it == index_.end()) {
            return;
        }
        eraseFrom(it->second, id);
        index_.erase(it);
    }

    // Reduce a resting order's remaining quantity by `qty` (a fill); erase it if that empties it.
    void reduce(OrderId id, Quantity qty) {
        const auto it = index_.find(id);
        if (it == index_.end()) {
            return;
        }
        const Location loc = it->second;
        auto* deq = deque(loc);
        if (deq == nullptr) {
            return;
        }
        for (MirrorOrder& o : *deq) {
            if (o.id == id) {
                o.remaining -= qty;
                if (o.remaining <= 0) {
                    eraseFrom(loc, id);
                    index_.erase(it);
                }
                return;
            }
        }
    }

    std::deque<MirrorOrder>* deque(const Location& loc) {
        if (loc.side == Side::Buy) {
            const auto it = bids_.find(loc.price);
            return it == bids_.end() ? nullptr : &it->second;
        }
        const auto it = asks_.find(loc.price);
        return it == asks_.end() ? nullptr : &it->second;
    }

    void eraseFrom(const Location& loc, OrderId id) {
        if (loc.side == Side::Buy) {
            eraseFromLevels(bids_, loc.price, id);
        } else {
            eraseFromLevels(asks_, loc.price, id);
        }
    }

    template<class LevelsT>
    static void eraseFromLevels(LevelsT& levels, Price price, OrderId id) {
        const auto lvlIt = levels.find(price);
        if (lvlIt == levels.end()) {
            return;
        }
        auto& deq = lvlIt->second;
        for (auto oi = deq.begin(); oi != deq.end(); ++oi) {
            if (oi->id == id) {
                deq.erase(oi);
                break;
            }
        }
        if (deq.empty()) {
            levels.erase(lvlIt);
        }
    }

    BidLevels bids_;
    Levels asks_;
    std::unordered_map<OrderId, Location> index_;

    Seq lastSeq_ = 0;
    Seq tradeCount_ = 0;
};

}  // namespace velox::marketdata
