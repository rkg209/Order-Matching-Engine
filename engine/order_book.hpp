#pragma once

#include "book/level_map.hpp"
#include "book/order_id_map.hpp"
#include "common/object_pool.hpp"
#include "common/types.hpp"
#include "engine/order.hpp"
#include "engine/trade.hpp"

namespace velox {

struct BookConfig {
    Price minPrice = 1 * kPriceScale;
    Price maxPrice = 1000 * kPriceScale;
    Price tick = kPriceScale / 100;  // 0.01
    std::size_t maxOrders = 1u << 20;
};

// A new limit order, as submitted. The book assigns `seq` itself, so callers cannot make
// priority nondeterministic by supplying their own.
struct NewOrder {
    OrderId id;
    Price price;
    Quantity quantity;
    ParticipantId participant;
    Side side;
};

enum class SubmitStatus : std::uint8_t {
    Ok = 0,
    RejectedPriceOutOfRange,
    RejectedInvalidQuantity,
    RejectedDuplicateId,
    RejectedPoolExhausted,  // backpressure -- NOT a fallback allocation (NFR-10)
};

// A single-instrument limit order book with price-time-priority matching.
//
// Spec 001 scope: LIMIT orders only. Market/IOC/FOK/cancel/replace/self-trade-prevention are
// Spec 002. The id map and the participant field exist already so that adding them is a
// change to the matching logic, not a change to the data structure.
class OrderBook {
 public:
    explicit OrderBook(const BookConfig& cfg = {});

    // Match `o` against the opposite side while it crosses, emitting a trade per fill into
    // `trades`; rest whatever quantity remains.
    SubmitStatus submit(const NewOrder& o, TradeBuffer& trades) noexcept;

    Price bestBid() const noexcept { return bids_.best(); }
    Price bestAsk() const noexcept { return asks_.best(); }

    // Aggregate resting quantity at a price, or 0. For tests and market data.
    Quantity quantityAt(Side side, Price price) noexcept;

    const book::OrderIdMap& orders() const noexcept { return idMap_; }
    Seq lastSeq() const noexcept { return seq_; }
    Seq tradeCount() const noexcept { return nextTradeId_; }
    std::size_t restingOrders() const noexcept { return idMap_.size(); }

 private:
    book::LevelMap& sideOf(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }

    BookConfig cfg_;
    book::LevelMap bids_;
    book::LevelMap asks_;
    book::OrderIdMap idMap_;
    ObjectPool<Order> pool_;

    Seq seq_ = 0;          // arrival counter == time priority
    Seq nextTradeId_ = 0;  // deterministic trade ids
};

}  // namespace velox
