#pragma once

#include "book/level_map.hpp"
#include "book/order_id_map.hpp"
#include "common/object_pool.hpp"
#include "common/types.hpp"
#include "engine/order.hpp"
#include "engine/trade.hpp"

namespace velox {

// LIMIT rests any unfilled residual. MARKET/IOC never rest -- an unfilled residual is
// cancelled. FOK is all-or-nothing, decided by a non-mutating pre-scan before any matching
// happens, so it can be rejected with the book provably untouched.
enum class OrderType : std::uint8_t { Limit = 0, Market, Ioc, Fok };

// What happens when an incoming order would trade against a resting order from the SAME
// participant. Applies to the aggressor, the resting order, or both; see order_book.cpp for
// the exact mechanics of each policy inside the match loop.
enum class StpPolicy : std::uint8_t { CancelAggressor = 0, CancelPassive, CancelBoth };

struct BookConfig {
    Price minPrice = 1 * kPriceScale;
    Price maxPrice = 1000 * kPriceScale;
    Price tick = kPriceScale / 100;  // 0.01
    std::size_t maxOrders = 1u << 20;
    StpPolicy stp = StpPolicy::CancelAggressor;
};

// A new order, as submitted. The book assigns `seq` itself, so callers cannot make priority
// nondeterministic by supplying their own. `price` is ignored for Market orders.
struct NewOrder {
    OrderId id;
    Price price;
    Quantity quantity;
    ParticipantId participant;
    Side side;
    OrderType type = OrderType::Limit;
};

enum class SubmitStatus : std::uint8_t {
    Ok = 0,
    RejectedPriceOutOfRange,
    RejectedInvalidQuantity,
    RejectedDuplicateId,
    RejectedPoolExhausted,           // backpressure -- NOT a fallback allocation (NFR-10)
    RejectedUnknownOrder,            // cancel/replace of an id that is not resting
    RejectedMarketIntoEmptyBook,     // MARKET with no opposite liquidity at all
    RejectedFokUnfillable,           // FOK pre-scan found insufficient liquidity; book untouched
    CancelledBySelfTradePrevention,  // STP fired; trades emitted before it stand
    CancelledResidual,               // MARKET/IOC residual was cancelled, not rested
};

// Enriched result for callers that need more than the status: how much traded, how much is
// left, and whether any quantity joined the book. A full execution-report stream belongs to
// the gateway/market-data consumers built in Spec 007/008; this is the minimum needed now.
struct OrderResult {
    SubmitStatus status = SubmitStatus::Ok;
    Quantity filled = 0;     // total quantity that traded on this command
    Quantity remaining = 0;  // unfilled at the end of the command
    bool rested = false;     // did any quantity join the book?
};

// A single-instrument order book with price-time-priority matching.
//
// Spec 001 scope was LIMIT orders only. Spec 002 widens this to MARKET / IOC / FOK, cancel,
// cancel-replace, and self-trade prevention. The id map and the participant field were built
// wide enough in Spec 001 that this is a change to the matching logic, not to the data
// structures.
class OrderBook {
 public:
    explicit OrderBook(const BookConfig& cfg = {});

    // Match `o` against the opposite side while it crosses, emitting a trade per fill into
    // `trades`; residual disposition depends on `o.type` (see OrderType).
    SubmitStatus submit(const NewOrder& o, TradeBuffer& trades) noexcept;
    OrderResult submitEx(const NewOrder& o, TradeBuffer& trades) noexcept;

    // Cancel a resting order by id. O(1). Rejects if the id is not currently resting -- which
    // covers "unknown id" and "already fully filled" with the same lookup, since a fully
    // filled order has already been erased from the id map.
    OrderResult cancel(OrderId id) noexcept;

    // Cancel `oldId` and submit `fresh` as a new arrival. Validates everything (old id exists,
    // new id not a duplicate, quantity/price valid) BEFORE touching the book, so a rejected
    // replace leaves the book completely untouched. Time priority resets: the replacement is a
    // genuinely new arrival (FR-10).
    OrderResult replace(OrderId oldId, const NewOrder& fresh, TradeBuffer& trades) noexcept;

    // Recovery restore path (Spec 006). NOT called from the matching hot path -- exclusively by
    // RecoveryManager and the shadow snapshot thread, which build a book by replaying a snapshot
    // body / journal tail rather than submit()ting through matching. No matching, no rejection
    // status, no logging: acquire from the pool, fill the order, rest it, index it. Every
    // resting order is by construction a LIMIT (MARKET/IOC never rest, FOK is all-or-nothing),
    // so there is no order-type argument. Kept out-of-line (order_book.cpp) so it cannot perturb
    // submit()'s codegen -- see .claude/plans/006-sequencer-journal-recovery.md, T5.
    //
    // `remaining` is passed separately from `o.quantity` because a restored order may already
    // be partially filled (the snapshot/journal records both `quantity` as originally submitted
    // and `remaining` as of the snapshot instant).
    //
    // Returns false on pool exhaustion or a duplicate id -- a caller restoring a snapshot/journal
    // that was itself produced under the SAME maxOrders cap should never see either, so a caller
    // MUST treat false as corruption, not silently drop the order: dropping it here is a worse
    // failure than a loud rejection (same principle as NFR-10's pool-exhaustion backpressure).
    bool restoreResting(const NewOrder& o, Quantity remaining, Seq seq) noexcept;

    // Recovery restore path (Spec 006). Sets the arrival/trade-id counters directly, bypassing
    // the ++seq_ increment submit() uses -- restoreResting() callers pass each order's own seq
    // explicitly, so this exists only to fix the book's OWN counters to match afterward.
    void restoreCounters(Seq lastSeq, Seq nextTradeId) noexcept;

    Price bestBid() const noexcept { return bids_.best(); }
    Price bestAsk() const noexcept { return asks_.best(); }

    // Aggregate resting quantity at a price, or 0. For tests and market data.
    Quantity quantityAt(Side side, Price price) noexcept;

    const book::OrderIdMap& orders() const noexcept { return idMap_; }
    Seq lastSeq() const noexcept { return seq_; }
    Seq tradeCount() const noexcept { return nextTradeId_; }
    std::size_t restingOrders() const noexcept { return idMap_.size(); }

    // Introspection accessors for the invariant checker (Spec 003) and market data (Spec 008).
    // Const, noexcept, non-virtual -- never touched by the matching hot path.
    const book::LevelMap& sideView(Side s) const noexcept { return s == Side::Buy ? bids_ : asks_; }
    const ObjectPool<Order>& pool() const noexcept { return pool_; }

 private:
    book::LevelMap& sideOf(Side s) noexcept { return s == Side::Buy ? bids_ : asks_; }

    // How the match loop stopped -- decides nothing by itself; residual disposition is decided
    // by the caller based on OrderType.
    enum class MatchOutcome : std::uint8_t { Exhausted, NoLongerCrosses, StpFired };

    // The core matching loop, shared by every order type. Consumes from `remaining`, emits
    // trades into `trades`. Does not touch the residual: resting/cancelling it is the caller's
    // job, because that is the one thing that differs by OrderType.
    MatchOutcome matchInto(const NewOrder& in, Seq mySeq, TradeBuffer& trades,
                           Quantity& remaining) noexcept;

    // Non-mutating pre-scan for FOK: how much quantity is actually reachable against `in`,
    // stopping at (not skipping past) a self-trade under CancelAggressor/CancelBoth, since
    // those orders can never legally be reached. Touches no pool, no level, no id map.
    Quantity availableAgainst(const NewOrder& in) const noexcept;

    // Shared residual-resting path (the LIMIT case, and the tail of cancel-replace). Returns
    // false on pool exhaustion (NFR-10 backpressure), which the caller must surface as
    // RejectedPoolExhausted rather than silently dropping the residual.
    bool restResidual(const NewOrder& in, Seq mySeq, Quantity remaining) noexcept;

    BookConfig cfg_;
    book::LevelMap bids_;
    book::LevelMap asks_;
    book::OrderIdMap idMap_;
    ObjectPool<Order> pool_;

    Seq seq_ = 0;          // arrival counter == time priority
    Seq nextTradeId_ = 0;  // deterministic trade ids
};

}  // namespace velox
