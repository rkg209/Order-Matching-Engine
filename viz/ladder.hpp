#pragma once

// Spec 010 T4: the visualizer's own top-N book, built ENTIRELY from the wire feed -- L2Delta for
// live level updates, L3Order only during a SnapshotStart/.../SnapshotEnd burst (there is no
// separate "L2 snapshot" message on the wire; Spec 008's snapshot burst is L3-only, so a level
// aggregate during a burst is built by folding each resting order's remaining qty into its price,
// the same way marketdata::BookMirror does for the engine-side mirror). Off the hot path entirely
// (this is the visualizer, a pure downstream consumer, off the read-only wire client) --
// allocates freely, same allowance every module outside engine/ and book/ gets.

#include <cstdint>
#include <deque>
#include <map>

#include "common/types.hpp"
#include "marketdata/feed_messages.hpp"
#include "protocol/message_types.hpp"

namespace velox::viz {

struct LevelAgg {
    Quantity qty = 0;
    std::uint32_t orders = 0;
};

struct TradePrint {
    Price price;
    Quantity qty;
    Side aggressorSide;
};

class Ladder {
 public:
    static constexpr std::size_t kMaxTrades = 50;

    void onSnapshotStart() {
        inBurst_ = true;
        bids_.clear();
        asks_.clear();
    }

    void onSnapshotEnd() { inBurst_ = false; }

    void onL3Order(const marketdata::L3OrderMsg& m) {
        if (!inBurst_ || m.action != protocol::L3Action::Rested) return;
        if (m.side == Side::Buy) {
            LevelAgg& agg = bids_[m.price];
            agg.qty += m.remaining;
            agg.orders += 1;
        } else {
            LevelAgg& agg = asks_[m.price];
            agg.qty += m.remaining;
            agg.orders += 1;
        }
    }

    void onL2Delta(const marketdata::L2DeltaMsg& m) {
        if (m.side == Side::Buy) {
            applyLevel(bids_, m);
        } else {
            applyLevel(asks_, m);
        }
    }

    void onTrade(const marketdata::TradeTickMsg& t) {
        trades_.push_back(TradePrint{t.price, t.quantity, t.aggressorSide});
        while (trades_.size() > kMaxTrades) {
            trades_.pop_front();
        }
        ++tradeCount_;
    }

    // Top-N, best->worst. `f(Price, Quantity, std::uint32_t orders)`.
    template<class F>
    void topBids(std::size_t n, F&& f) const {
        std::size_t i = 0;
        for (const auto& [price, agg] : bids_) {
            if (i++ >= n) break;
            f(price, agg.qty, agg.orders);
        }
    }

    template<class F>
    void topAsks(std::size_t n, F&& f) const {
        std::size_t i = 0;
        for (const auto& [price, agg] : asks_) {
            if (i++ >= n) break;
            f(price, agg.qty, agg.orders);
        }
    }

    const std::deque<TradePrint>& recentTrades() const noexcept { return trades_; }
    std::uint64_t tradeCount() const noexcept { return tradeCount_; }

 private:
    template<class LevelsT>
    static void applyLevel(LevelsT& levels, const marketdata::L2DeltaMsg& m) {
        if (m.action == protocol::L2Action::Delete) {
            levels.erase(m.price);
            return;
        }
        levels[m.price] = LevelAgg{m.totalQty, m.orderCount};
    }

    std::map<Price, LevelAgg, std::greater<Price>> bids_;
    std::map<Price, LevelAgg> asks_;
    std::deque<TradePrint> trades_;
    std::uint64_t tradeCount_ = 0;
    bool inBurst_ = false;
};

}  // namespace velox::viz
