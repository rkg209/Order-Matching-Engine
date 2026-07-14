#include "common/object_pool.hpp"

#include <gtest/gtest.h>

#include "engine/order.hpp"

using namespace velox;

TEST(ObjectPool, AcquireAndRelease) {
    ObjectPool<Order> pool(4);
    EXPECT_EQ(pool.capacity(), 4u);
    EXPECT_EQ(pool.available(), 4u);

    Order* a = pool.acquire();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(pool.inUse(), 1u);

    pool.release(a);
    EXPECT_EQ(pool.inUse(), 0u);
    EXPECT_EQ(pool.available(), 4u);
}

// Exhaustion MUST return null rather than allocating. This is NFR-10 and it is not a detail:
// a fallback allocation would be a hidden latency cliff that fires exactly when the system is
// under maximum load. A bounded rejection is strictly better than an unbounded stall.
TEST(ObjectPool, ExhaustionReturnsNullAndNeverAllocates) {
    ObjectPool<Order> pool(2);

    Order* a = pool.acquire();
    Order* b = pool.acquire();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    EXPECT_EQ(pool.available(), 0u);

    EXPECT_EQ(pool.acquire(), nullptr);  // exhausted -> backpressure, not a new allocation
    EXPECT_EQ(pool.acquire(), nullptr);  // and it stays that way

    pool.release(a);
    EXPECT_NE(pool.acquire(), nullptr);  // recovers once something is returned
}

TEST(ObjectPool, HandsOutDistinctObjects) {
    ObjectPool<Order> pool(8);
    Order* a = pool.acquire();
    Order* b = pool.acquire();
    Order* c = pool.acquire();
    EXPECT_NE(a, b);
    EXPECT_NE(b, c);
    EXPECT_NE(a, c);
}

TEST(ObjectPool, ReusesReleasedSlots) {
    ObjectPool<Order> pool(1);
    Order* a = pool.acquire();
    pool.release(a);
    Order* b = pool.acquire();
    EXPECT_EQ(a, b);  // same slot comes back
}

TEST(ObjectPool, ReleasingNullIsHarmless) {
    ObjectPool<Order> pool(2);
    pool.release(nullptr);
    EXPECT_EQ(pool.inUse(), 0u);
}
