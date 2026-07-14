#include "book/level_map.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace velox;
using namespace velox::book;

namespace {

constexpr Price kMin = 1 * kPriceScale;
constexpr Price kMax = 200 * kPriceScale;
constexpr Price kTick = kPriceScale / 100;  // 0.01

Price px(double p) {
    return static_cast<Price>(p * kPriceScale);
}

Order* mk(std::vector<Order>& store, OrderId id, Price price, Quantity qty, Side side) {
    Order o{};
    o.id = id;
    o.price = price;
    o.quantity = qty;
    o.remaining = qty;
    o.side = side;
    store.push_back(o);
    return &store.back();
}

}  // namespace

// The empty-book sentinels are load-bearing: they are what makes the crossing test false on an
// empty book with no special-case branch. If these regress to 0, the engine will happily trade
// against a book that is not there.
TEST(LevelMap, EmptySideReportsSentinel) {
    LevelMap bids(Side::Buy, kMin, kMax, kTick);
    LevelMap asks(Side::Sell, kMin, kMax, kTick);

    EXPECT_FALSE(bids.hasBest());
    EXPECT_FALSE(asks.hasBest());
    EXPECT_EQ(bids.best(), kBidEmpty);
    EXPECT_EQ(asks.best(), kAskEmpty);
}

TEST(LevelMap, BidBestIsHighestPrice) {
    LevelMap bids(Side::Buy, kMin, kMax, kTick);
    std::vector<Order> store;
    store.reserve(8);

    bids.addOrder(mk(store, 1, px(100), 10, Side::Buy));
    EXPECT_EQ(bids.best(), px(100));

    bids.addOrder(mk(store, 2, px(99), 10, Side::Buy));
    EXPECT_EQ(bids.best(), px(100));  // worse price does not become best

    bids.addOrder(mk(store, 3, px(101), 10, Side::Buy));
    EXPECT_EQ(bids.best(), px(101));  // better price does
}

TEST(LevelMap, AskBestIsLowestPrice) {
    LevelMap asks(Side::Sell, kMin, kMax, kTick);
    std::vector<Order> store;
    store.reserve(8);

    asks.addOrder(mk(store, 1, px(100), 10, Side::Sell));
    EXPECT_EQ(asks.best(), px(100));

    asks.addOrder(mk(store, 2, px(101), 10, Side::Sell));
    EXPECT_EQ(asks.best(), px(100));  // worse (higher) does not become best

    asks.addOrder(mk(store, 3, px(99), 10, Side::Sell));
    EXPECT_EQ(asks.best(), px(99));  // better (lower) does
}

// Emptying the best level is the one operation in this structure that is not O(1), and the one
// most likely to be got wrong. If the best price is not recovered correctly, the book will
// quote a price at which there is no liquidity.
TEST(LevelMap, EmptyingBestBidRecoversNextBest) {
    LevelMap bids(Side::Buy, kMin, kMax, kTick);
    std::vector<Order> store;
    store.reserve(8);

    Order* top = mk(store, 1, px(101), 10, Side::Buy);
    bids.addOrder(top);
    bids.addOrder(mk(store, 2, px(100), 10, Side::Buy));
    bids.addOrder(mk(store, 3, px(98), 10, Side::Buy));
    ASSERT_EQ(bids.best(), px(101));

    bids.levelAt(px(101))->unlink(top);
    bids.onLevelEmptied(px(101));
    EXPECT_EQ(bids.best(), px(100));  // next best DOWN, not the sentinel and not 98
}

TEST(LevelMap, EmptyingBestAskRecoversNextBest) {
    LevelMap asks(Side::Sell, kMin, kMax, kTick);
    std::vector<Order> store;
    store.reserve(8);

    Order* top = mk(store, 1, px(99), 10, Side::Sell);
    asks.addOrder(top);
    asks.addOrder(mk(store, 2, px(100), 10, Side::Sell));
    asks.addOrder(mk(store, 3, px(105), 10, Side::Sell));
    ASSERT_EQ(asks.best(), px(99));

    asks.levelAt(px(99))->unlink(top);
    asks.onLevelEmptied(px(99));
    EXPECT_EQ(asks.best(), px(100));  // next best UP
}

TEST(LevelMap, EmptyingAnInteriorLevelDoesNotDisturbBest) {
    LevelMap bids(Side::Buy, kMin, kMax, kTick);
    std::vector<Order> store;
    store.reserve(8);

    bids.addOrder(mk(store, 1, px(101), 10, Side::Buy));
    Order* mid = mk(store, 2, px(100), 10, Side::Buy);
    bids.addOrder(mid);
    ASSERT_EQ(bids.best(), px(101));

    bids.levelAt(px(100))->unlink(mid);
    bids.onLevelEmptied(px(100));
    EXPECT_EQ(bids.best(), px(101));  // unchanged
}

TEST(LevelMap, EmptyingTheLastLevelReturnsToSentinel) {
    LevelMap bids(Side::Buy, kMin, kMax, kTick);
    std::vector<Order> store;
    store.reserve(4);

    Order* only = mk(store, 1, px(100), 10, Side::Buy);
    bids.addOrder(only);
    ASSERT_TRUE(bids.hasBest());

    bids.levelAt(px(100))->unlink(only);
    bids.onLevelEmptied(px(100));

    EXPECT_FALSE(bids.hasBest());
    EXPECT_EQ(bids.best(), kBidEmpty);
}

// A gap between levels must be walked over, not stopped at. This is the case where a naive
// "just look at the adjacent tick" implementation silently reports no liquidity.
TEST(LevelMap, RecoversBestAcrossALargePriceGap) {
    LevelMap bids(Side::Buy, kMin, kMax, kTick);
    std::vector<Order> store;
    store.reserve(4);

    Order* top = mk(store, 1, px(150), 10, Side::Buy);
    bids.addOrder(top);
    bids.addOrder(mk(store, 2, px(20), 10, Side::Buy));  // far away
    ASSERT_EQ(bids.best(), px(150));

    bids.levelAt(px(150))->unlink(top);
    bids.onLevelEmptied(px(150));

    EXPECT_EQ(bids.best(), px(20));  // found across the gap
}

TEST(LevelMap, RangeChecking) {
    LevelMap bids(Side::Buy, kMin, kMax, kTick);
    EXPECT_TRUE(bids.inRange(px(100)));
    EXPECT_TRUE(bids.inRange(kMin));
    EXPECT_TRUE(bids.inRange(kMax));
    EXPECT_FALSE(bids.inRange(kMin - kTick));
    EXPECT_FALSE(bids.inRange(kMax + kTick));
    EXPECT_EQ(bids.levelAt(kMax + kTick), nullptr);
}
