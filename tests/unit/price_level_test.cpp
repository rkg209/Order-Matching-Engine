#include "engine/price_level.hpp"

#include <gtest/gtest.h>

#include <array>

using namespace velox;

namespace {

Order makeOrder(OrderId id, Quantity qty) {
    Order o{};
    o.id = id;
    o.quantity = qty;
    o.remaining = qty;
    o.price = 100 * kPriceScale;
    o.side = Side::Buy;
    return o;
}

}  // namespace

TEST(PriceLevel, StartsEmpty) {
    PriceLevel lvl;
    lvl.init(100 * kPriceScale);
    EXPECT_TRUE(lvl.empty());
    EXPECT_EQ(lvl.head(), nullptr);
    EXPECT_EQ(lvl.totalQuantity(), 0);
}

// Time priority is the whole reason this is a FIFO. If enqueue order and head order ever
// disagree, the engine is silently unfair and every other test would still pass.
TEST(PriceLevel, PreservesFifoArrivalOrder) {
    PriceLevel lvl;
    lvl.init(100 * kPriceScale);

    std::array<Order, 3> orders{makeOrder(1, 10), makeOrder(2, 20), makeOrder(3, 30)};
    for (auto& o : orders) {
        lvl.enqueue(&o);
    }

    EXPECT_EQ(lvl.count(), 3u);
    EXPECT_EQ(lvl.totalQuantity(), 60);

    // Walking head->tail must yield arrival order.
    Order* cur = lvl.head();
    ASSERT_NE(cur, nullptr);
    EXPECT_EQ(cur->id, 1);
    cur = cur->next;
    ASSERT_NE(cur, nullptr);
    EXPECT_EQ(cur->id, 2);
    cur = cur->next;
    ASSERT_NE(cur, nullptr);
    EXPECT_EQ(cur->id, 3);
    EXPECT_EQ(cur->next, nullptr);
    EXPECT_EQ(lvl.tail()->id, 3);
}

TEST(PriceLevel, UnlinkHead) {
    PriceLevel lvl;
    lvl.init(100 * kPriceScale);
    std::array<Order, 3> orders{makeOrder(1, 10), makeOrder(2, 20), makeOrder(3, 30)};
    for (auto& o : orders) lvl.enqueue(&o);

    lvl.unlink(&orders[0]);

    EXPECT_EQ(lvl.head()->id, 2);
    EXPECT_EQ(lvl.head()->prev, nullptr);
    EXPECT_EQ(lvl.count(), 2u);
    EXPECT_EQ(lvl.totalQuantity(), 50);
}

TEST(PriceLevel, UnlinkMiddle) {
    PriceLevel lvl;
    lvl.init(100 * kPriceScale);
    std::array<Order, 3> orders{makeOrder(1, 10), makeOrder(2, 20), makeOrder(3, 30)};
    for (auto& o : orders) lvl.enqueue(&o);

    lvl.unlink(&orders[1]);

    EXPECT_EQ(lvl.head()->id, 1);
    EXPECT_EQ(lvl.head()->next->id, 3);
    EXPECT_EQ(lvl.tail()->prev->id, 1);
    EXPECT_EQ(lvl.count(), 2u);
    EXPECT_EQ(lvl.totalQuantity(), 40);
}

TEST(PriceLevel, UnlinkTail) {
    PriceLevel lvl;
    lvl.init(100 * kPriceScale);
    std::array<Order, 3> orders{makeOrder(1, 10), makeOrder(2, 20), makeOrder(3, 30)};
    for (auto& o : orders) lvl.enqueue(&o);

    lvl.unlink(&orders[2]);

    EXPECT_EQ(lvl.tail()->id, 2);
    EXPECT_EQ(lvl.tail()->next, nullptr);
    EXPECT_EQ(lvl.count(), 2u);
    EXPECT_EQ(lvl.totalQuantity(), 30);
}

TEST(PriceLevel, UnlinkOnlyElementLeavesLevelEmpty) {
    PriceLevel lvl;
    lvl.init(100 * kPriceScale);
    Order o = makeOrder(1, 10);
    lvl.enqueue(&o);

    lvl.unlink(&o);

    EXPECT_TRUE(lvl.empty());
    EXPECT_EQ(lvl.head(), nullptr);
    EXPECT_EQ(lvl.tail(), nullptr);
    EXPECT_EQ(lvl.totalQuantity(), 0);
}

// A partial fill shrinks the order's remaining quantity, so the level's aggregate must shrink
// with it. If it does not, the level advertises liquidity that is not there -- and quantity
// conservation (invariant 1) breaks silently.
TEST(PriceLevel, ReduceQuantityTracksPartialFills) {
    PriceLevel lvl;
    lvl.init(100 * kPriceScale);
    Order o = makeOrder(1, 100);
    lvl.enqueue(&o);
    ASSERT_EQ(lvl.totalQuantity(), 100);

    o.remaining -= 30;
    lvl.reduceQuantity(30);
    EXPECT_EQ(lvl.totalQuantity(), 70);

    // ...and unlinking the partly-filled order removes exactly what is left, not the original.
    lvl.unlink(&o);
    EXPECT_EQ(lvl.totalQuantity(), 0);
}
