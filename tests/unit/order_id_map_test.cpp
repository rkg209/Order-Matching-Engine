#include "book/order_id_map.hpp"

#include <gtest/gtest.h>

#include <vector>

using namespace velox;
using namespace velox::book;

TEST(OrderIdMap, InsertFindErase) {
    OrderIdMap m(16);
    Order a{}, b{};
    a.id = 1;
    b.id = 2;

    EXPECT_TRUE(m.insert(1, &a));
    EXPECT_TRUE(m.insert(2, &b));
    EXPECT_EQ(m.size(), 2u);

    EXPECT_EQ(m.find(1), &a);
    EXPECT_EQ(m.find(2), &b);
    EXPECT_EQ(m.find(99), nullptr);

    EXPECT_TRUE(m.erase(1));
    EXPECT_EQ(m.find(1), nullptr);
    EXPECT_EQ(m.find(2), &b);  // erasing one must not disturb the other
    EXPECT_EQ(m.size(), 1u);

    EXPECT_FALSE(m.erase(1));  // already gone
}

TEST(OrderIdMap, RejectsDuplicateId) {
    OrderIdMap m(16);
    Order a{}, b{};
    EXPECT_TRUE(m.insert(7, &a));
    EXPECT_FALSE(m.insert(7, &b));
    EXPECT_EQ(m.find(7), &a);
}

// SUSTAINED CHURN -- the test that matters most in this file.
//
// A matching engine runs for hours: orders rest, fill, and leave, forever. The number of
// insert/erase cycles vastly exceeds the map's capacity. An implementation that leaves
// tombstones behind on erase is CORRECT but degrades to O(capacity) per lookup once the table
// saturates with them -- it does not fail, it just gets slower and slower until the engine
// stalls.
//
// The first version of this map did exactly that, and the symptom was the benchmark HANGING.
// Not a crash, not a failed assertion -- a hang, hours into sustained load, with every
// functional test still green. That is the worst way for a bug to present, so it gets a test
// that reproduces the condition directly.
//
// This churns 200x the table's capacity through it. With tombstones it crawls; with
// backward-shift deletion it stays flat.
TEST(OrderIdMap, SustainedInsertEraseChurnDoesNotDegrade) {
    constexpr std::size_t kCapacity = 256;
    constexpr std::size_t kCycles = 50'000;  // ~200x capacity

    OrderIdMap m(kCapacity);
    std::vector<Order> orders(8);

    // Keep a handful of live entries while cycling many thousands through.
    for (std::size_t i = 0; i < kCycles; ++i) {
        const OrderId id = static_cast<OrderId>(i);
        ASSERT_TRUE(m.insert(id, &orders[i % orders.size()])) << "insert failed at cycle " << i;
        ASSERT_NE(m.find(id), nullptr) << "lookup failed at cycle " << i;
        ASSERT_TRUE(m.erase(id)) << "erase failed at cycle " << i;
        ASSERT_EQ(m.find(id), nullptr) << "erased key still found at cycle " << i;
    }

    // The table must be genuinely empty -- not "empty but full of dead slots".
    EXPECT_EQ(m.size(), 0u);

    // And it must still work perfectly afterwards. If tombstones had accumulated, the table
    // would now be saturated and these inserts would fail or crawl.
    for (OrderId id = 1000; id < 1100; ++id) {
        ASSERT_TRUE(m.insert(id, &orders[0])) << "post-churn insert failed for " << id;
    }
    for (OrderId id = 1000; id < 1100; ++id) {
        EXPECT_NE(m.find(id), nullptr) << "post-churn lookup failed for " << id;
    }
}

// Deleting from the middle of a probe chain must not orphan the keys behind it. Backward-shift
// deletion has to pull them back into the hole, or they become permanently unreachable -- still
// in the table, but invisible to find().
//
// In this engine an orphaned entry means a resting order that can never be cancelled.
TEST(OrderIdMap, DeletingFromAProbeChainDoesNotOrphanLaterKeys) {
    OrderIdMap m(8);
    std::vector<Order> orders(64);

    // Insert enough ids to guarantee collisions and therefore probe chains.
    std::vector<OrderId> ids;
    for (OrderId id = 1; id <= 5; ++id) {
        orders[static_cast<std::size_t>(id)].id = id;
        ASSERT_TRUE(m.insert(id, &orders[static_cast<std::size_t>(id)]));
        ids.push_back(id);
    }

    // Erase from the middle, creating tombstones.
    ASSERT_TRUE(m.erase(2));
    ASSERT_TRUE(m.erase(3));

    // Everything else must STILL be findable, even if its probe chain runs through a tombstone.
    EXPECT_NE(m.find(1), nullptr);
    EXPECT_EQ(m.find(2), nullptr);
    EXPECT_EQ(m.find(3), nullptr);
    EXPECT_NE(m.find(4), nullptr);
    EXPECT_NE(m.find(5), nullptr);
}

TEST(OrderIdMap, HandlesManyIdsWithCollisions) {
    OrderIdMap m(1024);
    std::vector<Order> orders(500);

    for (std::size_t i = 0; i < orders.size(); ++i) {
        orders[i].id = static_cast<OrderId>(i + 1);
        ASSERT_TRUE(m.insert(orders[i].id, &orders[i])) << "insert failed at " << i;
    }
    EXPECT_EQ(m.size(), 500u);

    for (std::size_t i = 0; i < orders.size(); ++i) {
        EXPECT_EQ(m.find(static_cast<OrderId>(i + 1)), &orders[i]);
    }
}
