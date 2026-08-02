// Spec 010 T7.2: viz::Ladder against a hand-built sequence of wire messages -- one-sided book,
// fewer levels than N, more levels than N, and a level emptied by a trade (L2Delta Delete).

#include <gtest/gtest.h>

#include <vector>

#include "viz/ladder.hpp"

using namespace velox;
using namespace velox::viz;

namespace {

marketdata::L2DeltaMsg l2(Side side, protocol::L2Action action, Price price, Quantity qty,
                          std::uint32_t orders) {
    return marketdata::L2DeltaMsg{1, 1, side, action, price, qty, orders};
}

struct LevelRow {
    Price price;
    Quantity qty;
    std::uint32_t orders;
};

std::vector<LevelRow> collectBids(const Ladder& l, std::size_t n) {
    std::vector<LevelRow> out;
    l.topBids(n, [&](Price p, Quantity q, std::uint32_t o) { out.push_back({p, q, o}); });
    return out;
}

std::vector<LevelRow> collectAsks(const Ladder& l, std::size_t n) {
    std::vector<LevelRow> out;
    l.topAsks(n, [&](Price p, Quantity q, std::uint32_t o) { out.push_back({p, q, o}); });
    return out;
}

}  // namespace

TEST(Ladder, OneSidedBook) {
    Ladder l;
    l.onL2Delta(l2(Side::Buy, protocol::L2Action::Add, 100, 10, 1));
    l.onL2Delta(l2(Side::Buy, protocol::L2Action::Add, 99, 20, 2));

    const auto bids = collectBids(l, 12);
    ASSERT_EQ(bids.size(), 2u);
    EXPECT_EQ(bids[0].price, 100);  // best (highest) bid first
    EXPECT_EQ(bids[1].price, 99);
    EXPECT_TRUE(collectAsks(l, 12).empty());
}

TEST(Ladder, FewerLevelsThanTopN) {
    Ladder l;
    l.onL2Delta(l2(Side::Sell, protocol::L2Action::Add, 101, 5, 1));
    l.onL2Delta(l2(Side::Sell, protocol::L2Action::Add, 102, 7, 1));

    const auto asks = collectAsks(l, 12);
    ASSERT_EQ(asks.size(), 2u);
    EXPECT_EQ(asks[0].price, 101);  // best (lowest) ask first
    EXPECT_EQ(asks[1].price, 102);
}

TEST(Ladder, MoreLevelsThanTopNIsTruncated) {
    Ladder l;
    for (Price p = 90; p < 100; ++p) {
        l.onL2Delta(l2(Side::Buy, protocol::L2Action::Add, p, 1, 1));
    }
    const auto bids = collectBids(l, 3);
    ASSERT_EQ(bids.size(), 3u);
    EXPECT_EQ(bids[0].price, 99);
    EXPECT_EQ(bids[1].price, 98);
    EXPECT_EQ(bids[2].price, 97);
}

TEST(Ladder, LevelEmptiedByDeleteIsRemoved) {
    Ladder l;
    l.onL2Delta(l2(Side::Buy, protocol::L2Action::Add, 100, 10, 1));
    l.onL2Delta(l2(Side::Buy, protocol::L2Action::Add, 99, 5, 1));
    ASSERT_EQ(collectBids(l, 12).size(), 2u);

    l.onL2Delta(l2(Side::Buy, protocol::L2Action::Delete, 100, 0, 0));
    const auto bids = collectBids(l, 12);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids[0].price, 99);
}

TEST(Ladder, ModifyUpdatesAggregate) {
    Ladder l;
    l.onL2Delta(l2(Side::Sell, protocol::L2Action::Add, 100, 10, 1));
    l.onL2Delta(l2(Side::Sell, protocol::L2Action::Modify, 100, 6, 1));
    const auto asks = collectAsks(l, 12);
    ASSERT_EQ(asks.size(), 1u);
    EXPECT_EQ(asks[0].qty, 6);
}

TEST(Ladder, SnapshotBurstAggregatesL3OrdersPerLevel) {
    Ladder l;
    l.onSnapshotStart();
    l.onL3Order(
        marketdata::L3OrderMsg{1, 1, protocol::L3Action::Rested, 1, Side::Buy, 100, 5, 5, 1});
    l.onL3Order(
        marketdata::L3OrderMsg{1, 2, protocol::L3Action::Rested, 2, Side::Buy, 100, 3, 3, 2});
    l.onSnapshotEnd();

    const auto bids = collectBids(l, 12);
    ASSERT_EQ(bids.size(), 1u);
    EXPECT_EQ(bids[0].qty, 8);
    EXPECT_EQ(bids[0].orders, 2u);
}

TEST(Ladder, TradesRingCapsAtFiftyAndCountsAllTrades) {
    Ladder l;
    for (int i = 0; i < 60; ++i) {
        l.onTrade(
            marketdata::TradeTickMsg{1, static_cast<std::uint64_t>(i), i, 1, 2, 100, 1, Side::Buy});
    }
    EXPECT_EQ(l.tradeCount(), 60u);
    EXPECT_EQ(l.recentTrades().size(), Ladder::kMaxTrades);
}
