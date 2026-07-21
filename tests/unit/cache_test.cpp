#include "common/cache.hpp"

#include <gtest/gtest.h>

using namespace velox;

TEST(Cache, PaddedTypeIsCacheLineAligned) {
    EXPECT_EQ(alignof(CachePadded<int>), kCacheLineSize);
    EXPECT_EQ(sizeof(CachePadded<int>) % kCacheLineSize, 0u);
}

TEST(Cache, PaddedTypeHoldsItsValue) {
    CachePadded<int> p{42};
    EXPECT_EQ(p.value, 42);
}

TEST(Cache, TwoAdjacentInstancesDoNotShareACacheLine) {
    CachePadded<int> a[2];
    const auto* p0 = reinterpret_cast<const unsigned char*>(&a[0]);
    const auto* p1 = reinterpret_cast<const unsigned char*>(&a[1]);
    EXPECT_GE(static_cast<std::size_t>(p1 - p0), kCacheLineSize);
}
