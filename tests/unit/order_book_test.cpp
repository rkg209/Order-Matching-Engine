#include "engine/order_book.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace velox;

namespace {

Price px(double p) {
    return static_cast<Price>(p * kPriceScale);
}

class Book : public ::testing::Test {
 protected:
    Book() : book_(cfg()) {}

    static BookConfig cfg() {
        BookConfig c;
        c.minPrice = px(1);
        c.maxPrice = px(200);
        c.tick = kPriceScale / 100;
        c.maxOrders = 1024;
        return c;
    }

    // Submit and return the trades produced by THIS submission. Defaults `participant` to the
    // order's own id, so distinct orders are distinct participants by default (self-trade
    // prevention does not fire) unless a test explicitly passes a shared participant id to
    // exercise STP.
    TradeBuffer& submit(OrderId id, Side side, double price, Quantity qty,
                        ParticipantId participant = -1) {
        buf_.clear();
        NewOrder o{
            .id = id,
            .price = px(price),
            .quantity = qty,
            .participant = (participant == -1) ? id : participant,
            .side = side,
        };
        last_ = book_.submit(o, buf_);
        return buf_;
    }

    // Full-fidelity submit for the Spec 002 order types: returns the enriched OrderResult and
    // clears/reuses the same trade buffer as submit().
    OrderResult submitType(OrderId id, Side side, double price, Quantity qty, OrderType type,
                           ParticipantId participant = -1) {
        buf_.clear();
        NewOrder o{
            .id = id,
            .price = px(price),
            .quantity = qty,
            .participant = (participant == -1) ? id : participant,
            .side = side,
            .type = type,
        };
        const OrderResult r = book_.submitEx(o, buf_);
        last_ = r.status;
        return r;
    }

    OrderResult submitMarket(OrderId id, Side side, Quantity qty, ParticipantId participant = -1) {
        buf_.clear();
        NewOrder o{
            .id = id,
            .price = 0,
            .quantity = qty,
            .participant = (participant == -1) ? id : participant,
            .side = side,
            .type = OrderType::Market,
        };
        const OrderResult r = book_.submitEx(o, buf_);
        last_ = r.status;
        return r;
    }

    OrderResult replace(OrderId oldId, OrderId newId, Side side, double price, Quantity qty,
                        ParticipantId participant = -1) {
        buf_.clear();
        NewOrder fresh{
            .id = newId,
            .price = px(price),
            .quantity = qty,
            .participant = (participant == -1) ? newId : participant,
            .side = side,
        };
        const OrderResult r = book_.replace(oldId, fresh, buf_);
        last_ = r.status;
        return r;
    }

    OrderBook book_;
    std::array<Trade, 64> storage_{};
    TradeBuffer buf_{storage_.data(), storage_.size(), 0};
    SubmitStatus last_ = SubmitStatus::Ok;
};

}  // namespace

TEST_F(Book, NonCrossingOrderRestsAndTradesNothing) {
    auto& t = submit(1, Side::Buy, 100.0, 50);

    EXPECT_EQ(last_, SubmitStatus::Ok);
    EXPECT_EQ(t.count, 0u);
    EXPECT_EQ(book_.bestBid(), px(100));
    EXPECT_EQ(book_.bestAsk(), kAskEmpty);
    EXPECT_EQ(book_.quantityAt(Side::Buy, px(100)), 50);
}

// An order into an empty book must simply rest. The failure mode here is reading the opposite
// side's sentinel and "matching" at INT64_MAX -- the cheapest bug to write in a matching engine
// and the most embarrassing to ship.
TEST_F(Book, OrderIntoEmptyBookRestsAndDoesNotTouchTheSentinel) {
    auto& t = submit(1, Side::Sell, 100.0, 50);

    EXPECT_EQ(t.count, 0u);
    EXPECT_EQ(book_.bestAsk(), px(100));
    EXPECT_EQ(book_.bestBid(), kBidEmpty);
    EXPECT_EQ(book_.tradeCount(), 0);
}

// THE price rule. A buy at 101 hitting a resting sell at 100 trades at 100, not 101: price
// improvement accrues to the aggressor, because the resting order was there first and set the
// terms. Getting this backwards would let anyone extract value by quoting badly.
TEST_F(Book, TradeExecutesAtTheRestingOrdersPriceNotTheAggressors) {
    submit(1, Side::Sell, 100.0, 50);  // rests
    auto& t = submit(2, Side::Buy, 101.0, 50);

    ASSERT_EQ(t.count, 1u);
    EXPECT_EQ(t.data[0].price, px(100));  // <-- resting price
    EXPECT_EQ(t.data[0].quantity, 50);
    EXPECT_EQ(t.data[0].aggressorId, 2);
    EXPECT_EQ(t.data[0].passiveId, 1);
    EXPECT_EQ(t.data[0].aggressorSide, Side::Buy);

    // Both orders fully filled -> book is empty again.
    EXPECT_EQ(book_.bestBid(), kBidEmpty);
    EXPECT_EQ(book_.bestAsk(), kAskEmpty);
    EXPECT_EQ(book_.restingOrders(), 0u);
}

TEST_F(Book, ExactFillLeavesNothingResting) {
    submit(1, Side::Buy, 100.0, 30);
    auto& t = submit(2, Side::Sell, 100.0, 30);

    ASSERT_EQ(t.count, 1u);
    EXPECT_EQ(t.data[0].quantity, 30);
    EXPECT_EQ(book_.restingOrders(), 0u);
}

TEST_F(Book, AggressorPartiallyFilledRestsTheRemainderOnItsOwnSide) {
    submit(1, Side::Sell, 100.0, 30);           // only 30 available
    auto& t = submit(2, Side::Buy, 100.0, 50);  // wants 50

    ASSERT_EQ(t.count, 1u);
    EXPECT_EQ(t.data[0].quantity, 30);

    // The residual 20 rests as a BID at 100 -- on its own side. (The planning doc's pseudocode
    // rests unconditionally into the bid book, which happens to be right here and wrong for a
    // sell; see the sell case below.)
    EXPECT_EQ(book_.bestBid(), px(100));
    EXPECT_EQ(book_.quantityAt(Side::Buy, px(100)), 20);
    EXPECT_EQ(book_.bestAsk(), kAskEmpty);
}

// The direct test for the planning-doc bug: a residual SELL must rest in the ASKS.
TEST_F(Book, ResidualSellRestsInTheAsksNotTheBids) {
    submit(1, Side::Buy, 100.0, 30);
    auto& t = submit(2, Side::Sell, 100.0, 50);

    ASSERT_EQ(t.count, 1u);
    EXPECT_EQ(book_.bestAsk(), px(100));  // residual is an ASK
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(100)), 20);
    EXPECT_EQ(book_.bestBid(), kBidEmpty);  // and NOT a bid
    EXPECT_EQ(book_.quantityAt(Side::Buy, px(100)), 0);
}

TEST_F(Book, RestingOrderPartiallyFilledStaysWithReducedQuantity) {
    submit(1, Side::Sell, 100.0, 100);
    auto& t = submit(2, Side::Buy, 100.0, 40);

    ASSERT_EQ(t.count, 1u);
    EXPECT_EQ(t.data[0].quantity, 40);

    EXPECT_EQ(book_.bestAsk(), px(100));
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(100)), 60);  // 100 - 40
    EXPECT_EQ(book_.restingOrders(), 1u);
}

// Time priority. Two sells at the same price; the earlier one must fill first, always.
TEST_F(Book, FifoWithinAPriceLevel) {
    submit(1, Side::Sell, 100.0, 10);  // arrives first
    submit(2, Side::Sell, 100.0, 10);  // arrives second
    submit(3, Side::Sell, 100.0, 10);

    auto& t = submit(4, Side::Buy, 100.0, 25);

    ASSERT_EQ(t.count, 3u);
    EXPECT_EQ(t.data[0].passiveId, 1);  // earliest fills first
    EXPECT_EQ(t.data[0].quantity, 10);
    EXPECT_EQ(t.data[1].passiveId, 2);
    EXPECT_EQ(t.data[1].quantity, 10);
    EXPECT_EQ(t.data[2].passiveId, 3);
    EXPECT_EQ(t.data[2].quantity, 5);  // partial

    // Order 3 keeps its position with 5 left.
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(100)), 5);
}

// A partially filled resting order KEEPS its place at the head of the queue. It does not go to
// the back just because someone took part of it (FR-11). Only cancel/replace resets priority.
TEST_F(Book, PartiallyFilledRestingOrderKeepsItsQueuePosition) {
    submit(1, Side::Sell, 100.0, 100);  // first in queue
    submit(2, Side::Sell, 100.0, 50);   // second

    submit(3, Side::Buy, 100.0, 40);  // takes 40 from order 1; 60 left, still at the head

    auto& t = submit(4, Side::Buy, 100.0, 70);

    ASSERT_EQ(t.count, 2u);
    EXPECT_EQ(t.data[0].passiveId, 1);  // order 1 STILL fills first
    EXPECT_EQ(t.data[0].quantity, 60);  // its remaining 60
    EXPECT_EQ(t.data[1].passiveId, 2);  // then order 2
    EXPECT_EQ(t.data[1].quantity, 10);
}

// Price priority across levels: an aggressor sweeping the book takes the BEST prices first.
TEST_F(Book, SweepsMultipleLevelsInPriceOrder) {
    submit(1, Side::Sell, 102.0, 10);
    submit(2, Side::Sell, 100.0, 10);  // best ask
    submit(3, Side::Sell, 101.0, 10);

    auto& t = submit(4, Side::Buy, 102.0, 30);

    ASSERT_EQ(t.count, 3u);
    EXPECT_EQ(t.data[0].price, px(100));  // cheapest first
    EXPECT_EQ(t.data[1].price, px(101));
    EXPECT_EQ(t.data[2].price, px(102));

    EXPECT_EQ(book_.bestAsk(), kAskEmpty);  // book swept clean
    EXPECT_EQ(book_.restingOrders(), 0u);
}

TEST_F(Book, StopsAtTheLimitPriceAndRestsTheRest) {
    submit(1, Side::Sell, 100.0, 10);
    submit(2, Side::Sell, 105.0, 10);  // above the buyer's limit

    auto& t = submit(3, Side::Buy, 101.0, 30);

    ASSERT_EQ(t.count, 1u);  // only the 100 level is acceptable
    EXPECT_EQ(t.data[0].price, px(100));

    EXPECT_EQ(book_.bestAsk(), px(105));  // 105 untouched
    EXPECT_EQ(book_.bestBid(), px(101));  // residual 20 rests at the buyer's limit
    EXPECT_EQ(book_.quantityAt(Side::Buy, px(101)), 20);
}

TEST_F(Book, SellAggressorCrossesDownIntoBids) {
    submit(1, Side::Buy, 100.0, 10);
    submit(2, Side::Buy, 102.0, 10);  // best bid
    submit(3, Side::Buy, 101.0, 10);

    auto& t = submit(4, Side::Sell, 100.0, 30);

    ASSERT_EQ(t.count, 3u);
    EXPECT_EQ(t.data[0].price, px(102));  // seller takes the HIGHEST bids first
    EXPECT_EQ(t.data[1].price, px(101));
    EXPECT_EQ(t.data[2].price, px(100));
    EXPECT_EQ(book_.bestBid(), kBidEmpty);
}

TEST_F(Book, BookIsNeverLeftCrossed) {
    submit(1, Side::Sell, 100.0, 10);
    submit(2, Side::Buy, 105.0, 5);  // crosses, partially fills

    // After matching completes: bestBid < bestAsk, or a side is empty. Never crossed.
    const Price bid = book_.bestBid();
    const Price ask = book_.bestAsk();
    const bool oneSided = (bid == kBidEmpty) || (ask == kAskEmpty);
    EXPECT_TRUE(oneSided || bid < ask);
}

TEST_F(Book, QuantityIsConserved) {
    submit(1, Side::Sell, 100.0, 100);
    auto& t = submit(2, Side::Buy, 100.0, 60);

    Quantity traded = 0;
    for (std::size_t i = 0; i < t.count; ++i) traded += t.data[i].quantity;

    // Submitted 100 (sell) + 60 (buy) = 160.
    // Traded 60, consumed from BOTH sides -> 120. Resting: 40 (the sell's remainder).
    EXPECT_EQ(traded, 60);
    const Quantity resting =
        book_.quantityAt(Side::Sell, px(100)) + book_.quantityAt(Side::Buy, px(100));
    EXPECT_EQ(traded * 2 + resting, 160);
}

TEST_F(Book, RejectsDuplicateOrderId) {
    submit(1, Side::Buy, 100.0, 10);
    submit(1, Side::Buy, 101.0, 10);
    EXPECT_EQ(last_, SubmitStatus::RejectedDuplicateId);
}

TEST_F(Book, RejectsNonPositiveQuantity) {
    submit(1, Side::Buy, 100.0, 0);
    EXPECT_EQ(last_, SubmitStatus::RejectedInvalidQuantity);
    submit(2, Side::Buy, 100.0, -5);
    EXPECT_EQ(last_, SubmitStatus::RejectedInvalidQuantity);
}

TEST_F(Book, RejectsPriceOutOfRange) {
    submit(1, Side::Buy, 999.0, 10);
    EXPECT_EQ(last_, SubmitStatus::RejectedPriceOutOfRange);
}

// Trade ids must be a deterministic monotonic counter -- never a clock, never a UUID. Replay
// on a different day must produce identical trade ids or byte-identical golden replay is a lie.
TEST_F(Book, TradeIdsAreDeterministicAndMonotonic) {
    submit(1, Side::Sell, 100.0, 10);
    submit(2, Side::Sell, 100.0, 10);
    auto& t = submit(3, Side::Buy, 100.0, 20);

    ASSERT_EQ(t.count, 2u);
    EXPECT_EQ(t.data[0].id, 0);
    EXPECT_EQ(t.data[1].id, 1);
}

// ---------------------------------------------------------------------------------------------
// Spec 002 -- MARKET, IOC, FOK, cancel, cancel/replace, self-trade prevention.
// ---------------------------------------------------------------------------------------------

TEST_F(Book, MarketOrderTakesRestingLiquidityAtAnyPrice) {
    submit(1, Side::Sell, 100.0, 50);
    const OrderResult r = submitMarket(2, Side::Buy, 50);

    EXPECT_EQ(r.status, SubmitStatus::Ok);
    EXPECT_EQ(r.filled, 50);
    EXPECT_EQ(r.remaining, 0);
    EXPECT_FALSE(r.rested);
    EXPECT_EQ(book_.restingOrders(), 0u);
}

TEST_F(Book, MarketOrderIntoEmptyBookIsRejectedNotMatched) {
    const OrderResult r = submitMarket(1, Side::Buy, 10);

    EXPECT_EQ(r.status, SubmitStatus::RejectedMarketIntoEmptyBook);
    EXPECT_EQ(r.filled, 0);
    EXPECT_EQ(book_.bestBid(), kBidEmpty);
    EXPECT_EQ(book_.bestAsk(), kAskEmpty);
}

TEST_F(Book, MarketOrderResidualIsCancelledNotRested) {
    submit(1, Side::Sell, 100.0, 10);
    const OrderResult r = submitMarket(2, Side::Buy, 30);  // book only has 10

    EXPECT_EQ(r.status, SubmitStatus::CancelledResidual);
    EXPECT_EQ(r.filled, 10);
    EXPECT_EQ(r.remaining, 20);
    EXPECT_FALSE(r.rested);
    EXPECT_EQ(book_.restingOrders(), 0u);  // the 20 residual never rests
}

TEST_F(Book, IocFillsWhatItCanAndCancelsTheRest) {
    submit(1, Side::Sell, 100.0, 50);
    const OrderResult r = submitType(2, Side::Buy, 100.0, 80, OrderType::Ioc);

    EXPECT_EQ(r.status, SubmitStatus::CancelledResidual);
    EXPECT_EQ(r.filled, 50);
    EXPECT_EQ(r.remaining, 30);
    EXPECT_FALSE(r.rested);
    EXPECT_EQ(book_.restingOrders(), 0u);
}

TEST_F(Book, IocFullyFillableReportsOk) {
    submit(1, Side::Sell, 100.0, 50);
    const OrderResult r = submitType(2, Side::Buy, 100.0, 50, OrderType::Ioc);

    EXPECT_EQ(r.status, SubmitStatus::Ok);
    EXPECT_EQ(r.filled, 50);
    EXPECT_EQ(r.remaining, 0);
}

TEST_F(Book, FokFillsCompletelyWhenLiquiditySuffices) {
    submit(1, Side::Sell, 100.0, 50);
    submit(2, Side::Sell, 100.0, 50);
    const OrderResult r = submitType(3, Side::Buy, 100.0, 100, OrderType::Fok);

    EXPECT_EQ(r.status, SubmitStatus::Ok);
    EXPECT_EQ(r.filled, 100);
    EXPECT_EQ(r.remaining, 0);
    EXPECT_EQ(book_.restingOrders(), 0u);
}

// THE FOK edge case: needs 100, book has 99 -- must reject with the book PROVABLY untouched, not
// partially match and then roll back (matching is destructive; FOK cannot afford to start).
TEST_F(Book, FokRejectsWhenLiquidityIsOneShort) {
    submit(1, Side::Sell, 100.0, 99);
    const OrderResult r = submitType(2, Side::Buy, 100.0, 100, OrderType::Fok);

    EXPECT_EQ(r.status, SubmitStatus::RejectedFokUnfillable);
    EXPECT_EQ(r.filled, 0);
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(100)), 99);  // untouched
    EXPECT_EQ(book_.restingOrders(), 1u);
}

// STP interacting with the FOK pre-scan: under CancelAggressor, a same-participant order in the
// path can never legally be reached, so the pre-scan must STOP counting there, not skip past it
// to reach the (reachable-looking) liquidity behind it. Order 3 sits behind the self-trade and
// would wrongly make this look fillable if the scan skipped instead of stopped.
TEST_F(Book, FokPreScanStopsAtSelfTradeRatherThanSkippingPastIt) {
    submit(1, Side::Sell, 100.0, 30, /*participant=*/1);  // reachable
    submit(2, Side::Sell, 100.0, 50, /*participant=*/7);  // self-trade: stops the scan here
    submit(3, Side::Sell, 100.0, 50, /*participant=*/1);  // NOT reachable -- behind the collision

    const OrderResult r = submitType(4, Side::Buy, 100.0, 40, OrderType::Fok, /*participant=*/7);

    // Only order 1's 30 is actually reachable; 40 is wanted, so this must reject. A buggy
    // "skip" implementation would see 30 + 50 (order 3) = 80 and wrongly report it fillable.
    EXPECT_EQ(r.status, SubmitStatus::RejectedFokUnfillable);
    EXPECT_EQ(r.filled, 0);
    EXPECT_EQ(book_.restingOrders(), 3u);  // pre-scan is non-mutating: nothing touched
}

TEST_F(Book, SelfTradePreventionCancelsAggressorByDefault) {
    submit(1, Side::Sell, 100.0, 50, /*participant=*/9);
    const OrderResult r = submitType(2, Side::Buy, 100.0, 50, OrderType::Limit, /*participant=*/9);

    EXPECT_EQ(r.status, SubmitStatus::CancelledBySelfTradePrevention);
    EXPECT_EQ(r.filled, 0);
    EXPECT_FALSE(r.rested);
    // The resting order is untouched, and the aggressor never rests either.
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(100)), 50);
    EXPECT_EQ(book_.restingOrders(), 1u);
}

// Aggressor sweeps three resting orders at the same price; only the middle one shares its
// participant. CancelAggressor must stop AT that order, not skip over it to reach the third --
// stepping over it would let a later arrival fill ahead of an earlier one at the same price.
TEST_F(Book, SelfTradePreventionStopsAtNotPastTheCollision) {
    submit(1, Side::Sell, 100.0, 10, /*participant=*/1);
    submit(2, Side::Sell, 100.0, 10, /*participant=*/2);
    submit(3, Side::Sell, 100.0, 10, /*participant=*/3);

    const OrderResult r = submitType(4, Side::Buy, 100.0, 30, OrderType::Limit, /*participant=*/2);

    EXPECT_EQ(r.status, SubmitStatus::CancelledBySelfTradePrevention);
    EXPECT_EQ(r.filled, 10);                               // only order 1 traded
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(100)), 20);  // orders 2 and 3 both still resting
    EXPECT_EQ(book_.restingOrders(), 2u);
}

TEST_F(Book, CancelRemovesARestingOrder) {
    submit(1, Side::Sell, 100.0, 50);
    const OrderResult r = book_.cancel(1);

    EXPECT_EQ(r.status, SubmitStatus::Ok);
    EXPECT_EQ(r.remaining, 50);
    EXPECT_EQ(book_.restingOrders(), 0u);
    EXPECT_EQ(book_.bestAsk(), kAskEmpty);
}

TEST_F(Book, CancelUnknownIdIsRejected) {
    const OrderResult r = book_.cancel(999);
    EXPECT_EQ(r.status, SubmitStatus::RejectedUnknownOrder);
}

// An order the immediately preceding command fully filled has already been erased -- "unknown"
// and "already filled" are the same lookup, not two separate cases.
TEST_F(Book, CancelOfAnOrderFullyFilledByThePrecedingCommandIsRejected) {
    submit(1, Side::Sell, 100.0, 50);
    submit(2, Side::Buy, 100.0, 50);  // fully fills and erases order 1

    const OrderResult r = book_.cancel(1);
    EXPECT_EQ(r.status, SubmitStatus::RejectedUnknownOrder);
}

TEST_F(Book, CancelOfAPartiallyFilledOrderReportsFilledToDate) {
    submit(1, Side::Sell, 100.0, 100);
    submit(2, Side::Buy, 100.0, 40);  // partial fill, 60 remains

    const OrderResult r = book_.cancel(1);
    EXPECT_EQ(r.status, SubmitStatus::Ok);
    EXPECT_EQ(r.filled, 40);
    EXPECT_EQ(r.remaining, 60);
}

TEST_F(Book, ReplaceRejectsUnknownOldId) {
    const OrderResult r = replace(999, 2, Side::Buy, 100.0, 10);
    EXPECT_EQ(r.status, SubmitStatus::RejectedUnknownOrder);
}

TEST_F(Book, ReplaceLeavesBookUntouchedWhenRejected) {
    submit(1, Side::Sell, 100.0, 50);
    submit(2, Side::Sell, 101.0, 10);  // occupies id 2 already

    // Replacing order 1 with a duplicate of order 2's still-live id must be rejected, and order
    // 1 must still be exactly where it was.
    const OrderResult r = replace(1, 2, Side::Sell, 102.0, 10);

    EXPECT_EQ(r.status, SubmitStatus::RejectedDuplicateId);
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(100)), 50);  // order 1 untouched
    EXPECT_EQ(book_.restingOrders(), 2u);
}

TEST_F(Book, ReplaceToACrossingPriceMatchesInsteadOfResting) {
    submit(1, Side::Buy, 95.0, 30);
    submit(2, Side::Sell, 100.0, 50);

    const OrderResult r = replace(2, 3, Side::Sell, 94.0, 50);  // now crosses the bid at 95

    EXPECT_EQ(r.status, SubmitStatus::Ok);
    EXPECT_EQ(r.filled, 30);
    EXPECT_EQ(r.remaining, 20);
    EXPECT_EQ(book_.bestBid(), kBidEmpty);                // order 1 fully consumed
    EXPECT_EQ(book_.quantityAt(Side::Sell, px(94)), 20);  // residual rests at the new price
}

// FR-10's proof case: A rests, B rests later at the same price, A is replaced -- the replacement
// is a genuinely new arrival and must fill AFTER B, not resume A's old queue position.
TEST_F(Book, CancelReplaceResetsTimePriority) {
    submit(1, Side::Sell, 100.0, 10);
    submit(2, Side::Sell, 100.0, 10);
    replace(1, 3, Side::Sell, 100.0, 10);

    auto& t = submit(4, Side::Buy, 100.0, 20);

    ASSERT_EQ(t.count, 2u);
    EXPECT_EQ(t.data[0].passiveId, 2);  // B fills first now
    EXPECT_EQ(t.data[1].passiveId, 3);  // the replacement fills second
}

// NFR-22 made mechanical: total submitted quantity must equal total traded (counted on both
// sides) plus whatever is left resting, across a mixed sequence of order types.
TEST_F(Book, QuantityIsConservedAcrossMixedOrderTypes) {
    submit(1, Side::Sell, 100.0, 100);  // rests: 100

    Quantity traded = 0;
    // `submit()`/`submitType()` return a reference to the fixture's shared trade buffer, so each
    // one must be read before the NEXT submission clears and reuses it.
    auto& t1 = submit(2, Side::Buy, 100.0, 40);  // trades 40, rests 0 (IOC not used here)
    for (std::size_t i = 0; i < t1.count; ++i) traded += t1.data[i].quantity;

    const OrderResult r2 = submitType(3, Side::Buy, 100.0, 100, OrderType::Ioc);  // wants 100
    traded += r2.filled;

    // Submitted: 100 (order1) + 40 (order2) + 100 (order3, IOC).
    // Order1 fills 40 (to order2) then 60 (to order3's IOC) = 100 filled, 0 resting.
    // Order3's IOC fills 60 and cancels its remaining 40 (never rests).
    EXPECT_EQ(traded, 100);
    EXPECT_EQ(book_.restingOrders(), 0u);
    const Quantity resting = book_.quantityAt(Side::Sell, px(100));
    EXPECT_EQ(resting, 0);
}
