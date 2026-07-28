// Spec 008 DoD bullet 3 (non-blocking proof): a market-data consumer that never drains consumer
// index 1 at all -- the extreme case of "slow" -- must not stall the matching thread. Proven two
// ways, per the plan's explicit anti-cheat requirement:
//   (a) MulticastRing::lappedCount(1) > 0 -- index 1 really was lapped, not just idle because the
//       test was too short to matter.
//   (b) MatchingThread::fullSpins() stays low even though index 1 was never drained -- fullSpins_
//       only grows when tryClaim() finds the GATING minimum full (i.e. consumer 0's cursor,
//       continuously drained here by a stand-in for the exec-report router), so if GatingMask
//       were wrong and index 1 still gated, this would grow into the tens of thousands and fail.

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

#include "ipc/command.hpp"
#include "ipc/spsc_ring.hpp"
#include "runtime/matching_thread.hpp"

using namespace velox;

TEST(MarketDataSlowSubscriber, NonGatingConsumerNeverStallsMatchingThread) {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;

    ipc::SpscRing<ipc::Command> in;
    runtime::MatchingThread<>::OutRing out;
    runtime::MatchingThread<> mt(in, out, cfg);
    mt.start();

    // Stand-in for the exec-report router: the ONLY cursor that gates the producer. Drained
    // continuously so any fullSpins_ growth cannot be blamed on it.
    std::atomic<bool> stopDrain0{false};
    std::thread drainer0([&] {
        while (!stopDrain0.load(std::memory_order_acquire)) {
            if (out.tryPeek(0) == nullptr) {
                std::this_thread::yield();
            } else {
                out.consume(0);
            }
        }
    });

    // Index 1 (market data) is deliberately NEVER touched below -- that is the "slow subscriber"
    // this test proves is harmless. Cancels of a never-resting id: cheapest possible command that
    // still produces one OutboundEvent per dispatch, so the ring wraps (and laps index 1)
    // quickly without the pool-exhaustion bookkeeping a New-heavy workload would need.
    constexpr std::size_t kCommands = 300'000;  // several multiples of the 65536-slot ring
    const ipc::Command cancelUnknown{.id = 1,
                                     .newId = 0,
                                     .price = 0,
                                     .quantity = 0,
                                     .participant = 0,
                                     .kind = ipc::CommandKind::Cancel,
                                     .side = Side::Buy,
                                     .type = OrderType::Limit};
    for (std::size_t i = 0; i < kCommands; ++i) {
        while (!in.push(cancelUnknown)) {
            std::this_thread::yield();
        }
    }
    while (mt.processedCount() < kCommands) {
        std::this_thread::yield();
    }

    // lappedCount(1) only increments when something actually PEEKS at index 1 and finds itself
    // behind -- nothing has touched index 1 at all yet, which is the whole point of "slow
    // subscriber", so force exactly one peek here to observe what a real (even occasional)
    // market-data consumer would have detected on its own.
    ipc::OutboundEvent ev{};
    const auto status = out.tryPeekChecked(1, ev);
    EXPECT_EQ(status, decltype(out)::PeekStatus::Lapped)
        << "test was too short to actually lap the slow consumer";
    EXPECT_GT(out.lappedCount(1), 0u) << "test was too short to actually lap the slow consumer";
    EXPECT_LT(mt.fullSpins(), 1000u)
        << "matching thread backpressure-spun even though only the non-gating consumer lagged";

    stopDrain0.store(true, std::memory_order_release);
    drainer0.join();
    mt.stop();
}
