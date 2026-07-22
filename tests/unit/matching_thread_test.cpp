// Spec 005 T2: submit a known command sequence through the ring + matching thread, and assert
// the final book state matches the same sequence applied directly to an OrderBook.

#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/multicast_ring.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "runtime/matching_thread.hpp"

using namespace velox;
using namespace velox::ipc;
using namespace velox::runtime;

namespace {

BookConfig testConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;
    return cfg;
}

std::vector<Command> sampleSequence() {
    return {
        Command{.id = 1,
                .newId = 0,
                .price = 100 * kPriceScale,
                .quantity = 10,
                .participant = 1,
                .kind = CommandKind::New,
                .side = Side::Buy,
                .type = OrderType::Limit},
        Command{.id = 2,
                .newId = 0,
                .price = 101 * kPriceScale,
                .quantity = 5,
                .participant = 2,
                .kind = CommandKind::New,
                .side = Side::Sell,
                .type = OrderType::Limit},
        Command{.id = 3,
                .newId = 0,
                .price = 100 * kPriceScale,
                .quantity = 5,
                .participant = 3,
                .kind = CommandKind::New,
                .side = Side::Sell,
                .type = OrderType::Limit},
        Command{.id = 1,
                .newId = 0,
                .price = 0,
                .quantity = 0,
                .participant = 0,
                .kind = CommandKind::Cancel,
                .side = Side::Buy,
                .type = OrderType::Limit},
    };
}

}  // namespace

TEST(MatchingThread, MatchesDirectSubmissionResult) {
    // Direct: drive the same sequence straight into an OrderBook.
    OrderBook direct(testConfig());
    Trade storage[64];
    TradeBuffer buf{storage, 64, 0};
    for (const Command& c : sampleSequence()) {
        buf.clear();
        switch (c.kind) {
            case CommandKind::New:
                direct.submit(toNewOrder(c), buf);
                break;
            case CommandKind::Cancel:
                direct.cancel(c.id);
                break;
            case CommandKind::Replace:
                direct.replace(c.id, toNewOrder(c), buf);
                break;
        }
    }

    // Through the ring: same sequence, fed via the matching thread. The outbound ring is a
    // MulticastRing<OutboundEvent, 2> (Spec 005 T4's decision) -- both consumer cursors (the
    // future execution-report router and market-data publisher, Spec 007/008) must be drained,
    // since the producer gates on their minimum and this test's sequence is short enough that
    // an undrained cursor wouldn't otherwise show up as backpressure.
    SpscRing<Command> in;
    MatchingThread<>::OutRing out;
    MatchingThread<> mt(in, out, testConfig());
    mt.start();

    for (const Command& c : sampleSequence()) {
        while (!in.push(c)) {
        }
    }

    // Give the thread a chance to process everything before asking it to stop; stop() itself
    // only checks the flag once the inbound ring is empty, so nothing queued is lost.
    while (in.tryPeek() != nullptr) {
        std::this_thread::yield();
    }
    mt.stop();
    // Drain both consumer cursors so nothing is left stuck (nothing here asserts on outbound
    // content -- that is covered by the replay-through-the-ring suite).
    for (std::size_t idx = 0; idx < 2; ++idx) {
        while (out.tryPeek(idx) != nullptr) {
            out.consume(idx);
        }
    }

    EXPECT_EQ(mt.book().bestBid(), direct.bestBid());
    EXPECT_EQ(mt.book().bestAsk(), direct.bestAsk());
    EXPECT_EQ(mt.book().restingOrders(), direct.restingOrders());
}
