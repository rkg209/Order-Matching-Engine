// Spec 005 T0/T1: single-threaded correctness of SpscRing, plus the false-sharing checks the
// spec insists on MEASURING rather than assuming (`alignas` on paper proves nothing by itself).

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "common/cache.hpp"
#include "ipc/spsc_ring.hpp"

using namespace velox;
using namespace velox::ipc;

TEST(SpscRing, PushPopSingleThreaded) {
    SpscRing<int, 8> ring;
    int out = 0;
    EXPECT_FALSE(ring.pop(out));  // empty

    EXPECT_TRUE(ring.push(1));
    EXPECT_TRUE(ring.push(2));
    EXPECT_TRUE(ring.pop(out));
    EXPECT_EQ(out, 1);
    EXPECT_TRUE(ring.pop(out));
    EXPECT_EQ(out, 2);
    EXPECT_FALSE(ring.pop(out));
}

TEST(SpscRing, FullRingReturnsFalseAndDropsNothing) {
    SpscRing<int, 4> ring;
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(ring.push(i));
    }
    EXPECT_FALSE(ring.push(99));  // full: rejected, not silently dropped

    for (int i = 0; i < 4; ++i) {
        int out = -1;
        EXPECT_TRUE(ring.pop(out));
        EXPECT_EQ(out, i);
    }
}

TEST(SpscRing, WrapAround) {
    SpscRing<int, 4> ring;
    int out = 0;
    // Push/pop enough times to wrap the index several times over.
    for (int round = 0; round < 100; ++round) {
        EXPECT_TRUE(ring.push(round));
        EXPECT_TRUE(ring.pop(out));
        EXPECT_EQ(out, round);
    }
}

TEST(SpscRing, EmptyRingPeeksNull) {
    SpscRing<int, 4> ring;
    EXPECT_EQ(ring.tryPeek(), nullptr);
}

TEST(SpscRing, CursorsOnDifferentCacheLines) {
    SpscRing<int, 8> ring;
    // Runtime address check: measure it, don't just trust the alignas() worked.
    const auto headAddr = reinterpret_cast<std::uintptr_t>(ring.headCursorAddress());
    const auto tailAddr = reinterpret_cast<std::uintptr_t>(ring.tailCursorAddress());
    EXPECT_NE(headAddr / kCacheLineSize, tailAddr / kCacheLineSize);
}

// The load-bearing test: two real threads, one pushing a strict sequence, one popping it. Any
// SPSC bug (a missed release/acquire, a mis-cached cursor) shows up as a gap or a duplicate in
// the observed sequence -- nothing weaker catches it.
TEST(SpscRing, TwoThreadStrictSequenceNoGapsNoDuplicates) {
    constexpr std::uint64_t kN = 10'000'000;
    SpscRing<std::uint64_t, 4096> ring;

    std::vector<std::uint64_t> received;
    received.reserve(kN);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kN; ++i) {
            while (!ring.push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        std::uint64_t v = 0;
        std::uint64_t count = 0;
        while (count < kN) {
            if (ring.pop(v)) {
                received.push_back(v);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kN);
    for (std::uint64_t i = 0; i < kN; ++i) {
        ASSERT_EQ(received[i], i) << "gap or duplicate at index " << i;
    }
}
