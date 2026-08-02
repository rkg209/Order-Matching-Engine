// Spec 011 T9.2: a stalled shard does not touch its neighbour. The mechanical form of the spec's
// DoD bullet "a fault or a slow consumer in one shard does not affect another."
//
// Jams shard 0's outbound ring completely full WITHOUT ever draining it -- runtime/
// matching_thread.hpp's publishOutbound() spins on a full ring rather than dropping the event
// (backpressure, not data loss), so this deterministically makes shard 0's matching thread block
// inside dispatch(), observable via fullSpins() growing. While shard 0 is stuck, shard 1's
// matching thread must keep processing its own commands at its own pace and its own book must
// only ever reflect its OWN orders -- proving the two shards share no ring, no thread, no book.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "tests/shard/shard_test_helpers.hpp"

using namespace velox;
using namespace velox::shardtest;

namespace {

ipc::Command restingBuy(OrderId id, Price price) {
    ipc::Command c{};
    c.id = id;
    c.price = price;
    c.quantity = 1;
    c.participant = 1;
    c.kind = ipc::CommandKind::New;
    c.side = Side::Buy;
    c.type = OrderType::Limit;
    return c;
}

}  // namespace

TEST(ShardIsolation, StalledShardDoesNotStallNeighbour) {
    runtime::Shard stalled(/*instrumentId=*/1, isolationTestConfig(),
                           makeShardTestDir("isolation_stalled"), /*cpu=*/0);
    runtime::Shard healthy(/*instrumentId=*/2, isolationTestConfig(),
                           makeShardTestDir("isolation_healthy"), /*cpu=*/1);
    stalled.recoverAndStart();
    healthy.recoverAndStart();

    // > in-ring + out-ring capacity (65536 each) so the jam is guaranteed to fill both.
    constexpr int kJamCount = 150'000;
    std::atomic<bool> jamDone{false};
    std::thread jammer([&] {
        for (int i = 1; i <= kJamCount; ++i) {
            const Price price = (1 + (i % 1000)) * kPriceScale;
            ipc::Command c = restingBuy(static_cast<OrderId>(i), price);
            while (!stalled.inRing().push(c)) {
                std::this_thread::yield();
            }
        }
        jamDone.store(true, std::memory_order_release);
    });

    const auto jamDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (stalled.matching().fullSpins() == 0 && std::chrono::steady_clock::now() < jamDeadline) {
        std::this_thread::yield();
    }
    ASSERT_GT(stalled.matching().fullSpins(), 0u)
        << "failed to jam shard 0's outbound ring full within the deadline";

    // Real, verifiable work through the healthy shard WHILE shard 0 is stuck spinning. If the
    // two shards shared any ring, thread, or book, this would stall too.
    const std::size_t before = healthy.matching().processedCount();
    constexpr std::size_t kHealthyOrders = 50;
    for (OrderId id = 1; id <= static_cast<OrderId>(kHealthyOrders); ++id) {
        ipc::Command c = restingBuy(id, static_cast<Price>(id) * kPriceScale);
        while (!healthy.inRing().push(c)) {
            std::this_thread::yield();
        }
    }

    const auto healthyDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (healthy.matching().processedCount() < before + kHealthyOrders &&
           std::chrono::steady_clock::now() < healthyDeadline) {
        // Consumer 0 is non-blocking here on purpose -- draining it is what a real router
        // thread would do, and it must never depend on shard 0 in any way.
        while (healthy.outRing().tryPeek(0) != nullptr) {
            healthy.outRing().consume(0);
        }
        std::this_thread::yield();
    }
    EXPECT_EQ(healthy.matching().processedCount(), before + kHealthyOrders)
        << "neighbour shard's matching thread was stalled by shard 0's full outbound ring";
    // Every order that rested is visible only in ITS OWN book -- the highest id (50) priced
    // highest is the best bid, and it is exactly what THIS shard was sent, nothing from shard 0.
    EXPECT_EQ(healthy.matching().book().bestBid(),
              static_cast<Price>(kHealthyOrders) * kPriceScale);
    EXPECT_EQ(healthy.matching().book().restingOrders(), kHealthyOrders);

    // Teardown: unstick shard 0 by draining its outbound ring so its matching thread (and the
    // jammer thread still trying to push the tail of kJamCount) can both finish.
    while (!jamDone.load(std::memory_order_acquire)) {
        while (stalled.outRing().tryPeek(0) != nullptr) {
            stalled.outRing().consume(0);
        }
        std::this_thread::yield();
    }
    jammer.join();
    while (stalled.outRing().tryPeek(0) != nullptr) {
        stalled.outRing().consume(0);
    }

    stalled.stop();
    healthy.stop();
}
