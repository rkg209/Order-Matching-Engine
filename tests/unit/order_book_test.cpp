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

    // Submit and return the trades produced by THIS submission.
    TradeBuffer& submit(OrderId id, Side side, double price, Quantity qty,
                        ParticipantId participant = 1) {
        buf_.clear();
        NewOrder o{
            .id = id,
            .price = px(price),
            .quantity = qty,
            .participant = participant,
            .side = side,
        };
        last_ = book_.submit(o, buf_);
        return buf_;
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
