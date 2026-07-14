#include "engine/order_book.hpp"

#include <algorithm>

namespace velox {

OrderBook::OrderBook(const BookConfig& cfg)
    : cfg_(cfg),
      bids_(Side::Buy, cfg.minPrice, cfg.maxPrice, cfg.tick),
      asks_(Side::Sell, cfg.minPrice, cfg.maxPrice, cfg.tick),
      idMap_(cfg.maxOrders * 2),  // keep the load factor low; probing degrades badly above ~0.7
      pool_(cfg.maxOrders) {}

Quantity OrderBook::quantityAt(Side side, Price price) noexcept {
    PriceLevel* lvl = sideOf(side).levelAt(price);
    return lvl == nullptr ? 0 : lvl->totalQuantity();
}

namespace {

// Does an incoming order at `price` cross a resting order at `restingPrice`?
//   BUY  crosses when it is willing to pay at least the ask.
//   SELL crosses when it is willing to accept at most the bid.
//
// On an empty book the opposite side's best is a sentinel (kAskEmpty = INT64_MAX for asks,
// kBidEmpty = INT64_MIN for bids), and both comparisons are then false without a guard. That
// is the whole reason those sentinel values were chosen.
inline bool crosses(Side side, Price price, Price restingPrice) noexcept {
    return side == Side::Buy ? (price >= restingPrice) : (price <= restingPrice);
}

}  // namespace

SubmitStatus OrderBook::submit(const NewOrder& in, TradeBuffer& trades) noexcept {
    if (in.quantity <= 0) {
        return SubmitStatus::RejectedInvalidQuantity;
    }
    if (!bids_.inRange(in.price)) {
        return SubmitStatus::RejectedPriceOutOfRange;
    }
    if (idMap_.find(in.id) != nullptr) {
        return SubmitStatus::RejectedDuplicateId;
    }

    const Seq mySeq = ++seq_;
    Quantity remaining = in.quantity;

    // NOTE: this branches on `in.side`. planning/03-system-design.md's pseudocode does not --
    // it rests into the bid book and clears levels from the ask book unconditionally, so a
    // resting SELL would land in the bids. That bug is why this loop is written from the
    // semantics in .claude/skills/matching-semantics, not transcribed from the doc.
    book::LevelMap& opposite = sideOf(velox::opposite(in.side));
    book::LevelMap& own = sideOf(in.side);

    // ---- Match while crossing -------------------------------------------------------------
    while (remaining > 0 && opposite.hasBest()) {
        const Price bestOpp = opposite.best();
        if (!crosses(in.side, in.price, bestOpp)) {
            break;  // the best opposite price is no longer acceptable; nothing further can be
        }

        PriceLevel* level = opposite.levelAt(bestOpp);
        while (remaining > 0 && !level->empty()) {
            Order* resting = level->head();  // earliest arrival at this price -- FIFO
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

    // ---- Rest the residual ----------------------------------------------------------------
    if (remaining > 0) {
        Order* o = pool_.acquire();
        if (o == nullptr) {
            // Pool exhausted. Reject -- do NOT fall back to allocating (NFR-10). A fallback
            // allocation would be a latency cliff that fires exactly when load is highest.
            // The trades emitted above still stand; this order's residual simply does not rest.
            return SubmitStatus::RejectedPoolExhausted;
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

        own.addOrder(o);  // rests on its OWN side, at its own limit price
        idMap_.insert(in.id, o);
    }

    return SubmitStatus::Ok;
}

}  // namespace velox
