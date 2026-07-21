#include "engine/order_book.hpp"

#include <algorithm>
#include <bit>

#include "platform/platform.hpp"

namespace velox {

namespace {

// OrderIdMap requires a power-of-two capacity (it masks with capacity-1 instead of taking a
// modulus). Nothing validated that before this -- a BookConfig.maxOrders of, say, 1000 would
// silently produce a broken mask (1999 is not a power of two) and every probe would corrupt.
// Startup-only, off the hot path: called once, from the constructor.
std::size_t nextPowerOfTwo(std::size_t n) noexcept {
    if (n <= 1) return 1;
    return std::size_t{1} << (std::bit_width(n - 1));
}

}  // namespace

OrderBook::OrderBook(const BookConfig& cfg)
    : cfg_(cfg),
      bids_(Side::Buy, cfg.minPrice, cfg.maxPrice, cfg.tick),
      asks_(Side::Sell, cfg.minPrice, cfg.maxPrice, cfg.tick),
      // keep the load factor low; probing degrades badly above ~0.7 -- and round up to a power
      // of two, which OrderIdMap's mask-based probing requires (see nextPowerOfTwo() above).
      idMap_(nextPowerOfTwo(cfg.maxOrders * 2)),
      pool_(cfg.maxOrders) {
    // Startup-only, off the hot path. Locks the pages this process has touched so far into
    // physical memory, ahead of the first order -- a page fault mid-match is a syscall in the
    // middle of a matching decision, invisible in the mean and brutal in the p999. Honestly a
    // no-op on macOS (see platform::prefaultPages()); real on Linux, the benchmark target.
    platform::prefaultPages();
}

Quantity OrderBook::quantityAt(Side side, Price price) noexcept {
    PriceLevel* lvl = sideOf(side).levelAt(price);
    return lvl == nullptr ? 0 : lvl->totalQuantity();
}

namespace {

// Does an incoming order at `price` cross a resting order at `restingPrice`?
//   BUY  crosses when it is willing to pay at least the ask.
//   SELL crosses when it is willing to accept at most the bid.
//   MARKET crosses any resting price -- it has no limit. The empty-book case is NOT handled
//   here (this always returns true for Market); it is the loop's `opposite.hasBest()` guard
//   that stops a market order from "matching" a sentinel on an empty book.
//
// On an empty book the opposite side's best is a sentinel (kAskEmpty = INT64_MAX for asks,
// kBidEmpty = INT64_MIN for bids), and both LIMIT comparisons are then false without a guard.
// That is the whole reason those sentinel values were chosen.
inline bool crosses(OrderType type, Side side, Price price, Price restingPrice) noexcept {
    if (type == OrderType::Market) {
        return true;
    }
    return side == Side::Buy ? (price >= restingPrice) : (price <= restingPrice);
}

}  // namespace

OrderBook::MatchOutcome OrderBook::matchInto(const NewOrder& in, Seq /*mySeq*/, TradeBuffer& trades,
                                             Quantity& remaining) noexcept {
    // NOTE: this branches on `in.side`. planning/03-system-design.md's pseudocode does not --
    // it rests into the bid book and clears levels from the ask book unconditionally, so a
    // resting SELL would land in the bids. That bug is why this loop is written from the
    // semantics in .claude/skills/matching-semantics, not transcribed from the doc.
    book::LevelMap& opposite = sideOf(velox::opposite(in.side));

    while (remaining > 0 && opposite.hasBest()) {
        const Price bestOpp = opposite.best();
        if (!crosses(in.type, in.side, in.price, bestOpp)) {
            return MatchOutcome::NoLongerCrosses;  // the best opposite price is no longer
                                                   // acceptable; nothing further can be either.
        }

        PriceLevel* level = opposite.levelAt(bestOpp);
        while (remaining > 0 && !level->empty()) {
            Order* resting = level->head();  // earliest arrival at this price -- FIFO

            // Self-trade prevention. Checked BEFORE emitting a trade, never by skipping over
            // the resting order to reach the one behind it -- doing that would let a later
            // arrival fill ahead of an earlier one at the same price, breaking the one
            // invariant the whole book is built around.
            if (resting->participant == in.participant) {
                const StpPolicy policy = cfg_.stp;
                if (policy == StpPolicy::CancelPassive || policy == StpPolicy::CancelBoth) {
                    Order* victim = resting;
                    level->unlink(victim);
                    idMap_.erase(victim->id);
                    pool_.release(victim);
                    if (level->empty()) {
                        opposite.onLevelEmptied(bestOpp);
                    }
                    if (policy == StpPolicy::CancelPassive) {
                        continue;  // queue behind it becomes reachable
                    }
                }
                // CancelAggressor or CancelBoth: stop matching entirely. The residual does not
                // rest; trades already emitted this command stand.
                return MatchOutcome::StpFired;
            }

            const Quantity qty = std::min(remaining, resting->remaining);

            trades.push(Trade{
                .id = nextTradeId_++,
                .aggressorId = in.id,
                .passiveId = resting->id,
                // The RESTING order's price, not the aggressor's: price improvement accrues to
                // the aggressor. A buy at 101 into a resting sell at 100 trades at 100.
                .price = resting->price,
                .quantity = qty,
                .aggressorSide = in.side,
            });

            remaining -= qty;
            resting->remaining -= qty;

            // `qty` has left this level, whether the resting order was fully or partly filled.
            // Account for it unconditionally, BEFORE any unlink.
            //
            // Doing this only on the partial-fill path was a real bug (caught by
            // Book.FifoWithinAPriceLevel): unlink() decrements the level's aggregate by
            // o->remaining, which is already 0 for a fully-filled order -- so a full fill
            // subtracted nothing, and the level went on advertising liquidity that had already
            // traded away. Quantity conservation broke silently.
            level->reduceQuantity(qty);

            if (resting->remaining == 0) {
                // Fully filled: leave the book entirely. unlink() now subtracts its remaining,
                // which is 0, so the aggregate stays correct.
                level->unlink(resting);
                idMap_.erase(resting->id);
                pool_.release(resting);
            }
            // Otherwise it stays exactly where it is. A partially filled order KEEPS its place
            // at the head of the queue (FR-11) -- being partly taken does not cost it time
            // priority. Only a cancel/replace resets that.
        }

        if (level->empty()) {
            opposite.onLevelEmptied(bestOpp);  // recover the next best price on that side
        }
    }

    return MatchOutcome::Exhausted;
}

Quantity OrderBook::availableAgainst(const NewOrder& in) const noexcept {
    const book::LevelMap& opposite = (in.side == Side::Buy) ? asks_ : bids_;

    Quantity sum = 0;
    Price p = opposite.best();
    const Price sentinel = emptySentinel(velox::opposite(in.side));
    while (p != sentinel) {
        if (!crosses(in.type, in.side, in.price, p)) {
            break;
        }
        const PriceLevel* level = opposite.levelAt(p);
        for (const Order* o = level->head(); o != nullptr; o = o->next) {
            // Under CancelAggressor/CancelBoth, this order can never legally be reached: STP
            // would fire the instant matching got here, so counting its quantity would report
            // liquidity FOK could never actually take, and then fail mid-execution -- the exact
            // destructive-mutation situation the pre-scan exists to avoid. Stop, don't skip.
            if (o->participant == in.participant) {
                if (cfg_.stp == StpPolicy::CancelPassive) {
                    continue;  // it will be removed; the order behind it is reachable
                }
                return sum;
            }
            sum += o->remaining;
            if (sum >= in.quantity) {
                return sum;  // already enough; no need to keep walking
            }
        }
        p = opposite.nextOccupied(p);
    }
    return sum;
}

bool OrderBook::restResidual(const NewOrder& in, Seq mySeq, Quantity remaining) noexcept {
    Order* o = pool_.acquire();
    if (o == nullptr) {
        // Pool exhausted. The caller must turn this into RejectedPoolExhausted (NFR-10), NOT
        // silently drop the residual -- a dropped order that reports Ok is a worse failure than
        // a loud rejection: it is quantity conservation broken invisibly.
        return false;
    }
    o->id = in.id;
    o->price = in.price;
    o->quantity = in.quantity;
    o->remaining = remaining;
    o->participant = in.participant;
    o->seq = mySeq;
    o->side = in.side;
    o->prev = nullptr;
    o->next = nullptr;
    o->level = nullptr;

    sideOf(in.side).addOrder(o);  // rests on its OWN side, at its own limit price
    idMap_.insert(in.id, o);
    return true;
}

OrderResult OrderBook::submitEx(const NewOrder& in, TradeBuffer& trades) noexcept {
    OrderResult result{};

    if (in.quantity <= 0) {
        result.status = SubmitStatus::RejectedInvalidQuantity;
        return result;
    }
    // Market orders carry no limit price -- never read in.price for range validation, since it
    // may be garbage and indexing on it would go out of bounds.
    if (in.type != OrderType::Market && !bids_.inRange(in.price)) {
        result.status = SubmitStatus::RejectedPriceOutOfRange;
        return result;
    }
    if (idMap_.find(in.id) != nullptr) {
        result.status = SubmitStatus::RejectedDuplicateId;
        return result;
    }
    if (in.type == OrderType::Market && !sideOf(velox::opposite(in.side)).hasBest()) {
        result.status = SubmitStatus::RejectedMarketIntoEmptyBook;
        return result;
    }

    if (in.type == OrderType::Fok) {
        if (availableAgainst(in) < in.quantity) {
            result.status = SubmitStatus::RejectedFokUnfillable;
            return result;
        }
    }

    const Seq mySeq = ++seq_;
    Quantity remaining = in.quantity;

    const MatchOutcome outcome = matchInto(in, mySeq, trades, remaining);
    result.filled = in.quantity - remaining;
    result.remaining = remaining;

    if (outcome == MatchOutcome::StpFired) {
        result.status = SubmitStatus::CancelledBySelfTradePrevention;
        return result;
    }

    switch (in.type) {
        case OrderType::Limit:
            if (remaining > 0) {
                if (!restResidual(in, mySeq, remaining)) {
                    // Trades emitted above still stand (NFR-10); only the residual is rejected.
                    result.status = SubmitStatus::RejectedPoolExhausted;
                    break;
                }
                result.rested = true;
            }
            result.status = SubmitStatus::Ok;
            break;
        case OrderType::Market:
        case OrderType::Ioc:
            result.status = (remaining > 0) ? SubmitStatus::CancelledResidual : SubmitStatus::Ok;
            break;
        case OrderType::Fok:
            // The pre-scan guarantees remaining == 0 here, modulo STP firing mid-match -- which
            // the pre-scan already accounted for by stopping its sum at the same order.
            result.status =
                (remaining > 0) ? SubmitStatus::RejectedFokUnfillable : SubmitStatus::Ok;
            break;
    }

    return result;
}

SubmitStatus OrderBook::submit(const NewOrder& in, TradeBuffer& trades) noexcept {
    return submitEx(in, trades).status;
}

OrderResult OrderBook::cancel(OrderId id) noexcept {
    OrderResult result{};

    Order* o = idMap_.find(id);
    if (o == nullptr) {
        // Covers both "never existed" and "already fully filled and erased" -- there is no
        // separate filled-order table, so these are literally the same lookup.
        result.status = SubmitStatus::RejectedUnknownOrder;
        return result;
    }

    result.remaining = o->remaining;
    result.filled = o->quantity - o->remaining;

    const Price price = o->price;
    const Side side = o->side;

    // Order matters: unlink() reads o->remaining, release() invalidates the object.
    PriceLevel* level = o->level;
    level->unlink(o);
    if (level->empty()) {
        // Only recompute best if THIS level is now empty. Calling onLevelEmptied()
        // unconditionally (the previous bug here, caught by Spec 003's I7 invariant) skips past
        // a still-occupied level at `price` whenever it happened to be the best -- e.g.
        // cancelling one of two orders resting at the same price wrongly walked past that price
        // looking for the next occupied level, even though the other order was still there.
        sideOf(side).onLevelEmptied(price);
    }
    idMap_.erase(id);
    pool_.release(o);

    result.status = SubmitStatus::Ok;
    return result;
}

OrderResult OrderBook::replace(OrderId oldId, const NewOrder& fresh, TradeBuffer& trades) noexcept {
    OrderResult result{};

    // Validate everything BEFORE touching the book -- a rejected replace must leave the book
    // completely untouched.
    Order* old = idMap_.find(oldId);
    if (old == nullptr) {
        result.status = SubmitStatus::RejectedUnknownOrder;
        return result;
    }
    if (fresh.quantity <= 0) {
        result.status = SubmitStatus::RejectedInvalidQuantity;
        return result;
    }
    if (fresh.type != OrderType::Market && !bids_.inRange(fresh.price)) {
        result.status = SubmitStatus::RejectedPriceOutOfRange;
        return result;
    }
    if (fresh.id != oldId && idMap_.find(fresh.id) != nullptr) {
        result.status = SubmitStatus::RejectedDuplicateId;
        return result;
    }

    // cancel() looks up oldId again, which is fine: this is not the hot path (Spec 002 is a
    // correctness-breadth spec, not a latency one), and it keeps cancel() the single place that
    // knows how to unlink an order.
    cancel(oldId);
    return submitEx(fresh, trades);
}

}  // namespace velox
