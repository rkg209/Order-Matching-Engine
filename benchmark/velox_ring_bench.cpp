// Spec 005 benchmark: end-to-end saturation throughput + ring-transit latency (T5), and the A/B
// on the outbound design (T4).
//
// SCOPE NOTE, same discipline as velox_bench.cpp:
//   Saturation, not rate-driven -- a producer thread pushes as fast as it possibly can. There is
//   no intended-schedule generator here, so coordinated omission is ABSENT, not corrected for.
//   Spec 009 is where the rate-driven, CO-corrected methodology becomes mandatory.
//   No journal, no durability -- this is a no-durability number and must always be reported as
//   one (constitution Principle 6).

#include <hdr/hdr_histogram.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/multicast_ring.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "platform/platform.hpp"
#include "runtime/matching_thread.hpp"

using namespace velox;

namespace {

constexpr Price kMid = 100 * kPriceScale;
constexpr Price kTick = kPriceScale / 100;

BookConfig makeConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kTick;
    cfg.maxOrders = 1u << 20;
    return cfg;
}

// Same steady-state rest/cross alternation as velox_bench.cpp's BM_SubmitRestingOrder: net pool
// usage per pair is zero, so the book never grows and the pool is never exhausted. Copied
// deliberately (progress_report.md [005] already paid for the lesson that a benchmark which
// degrades into measuring RejectedPoolExhausted is worse than none).
ipc::Command steadyStateCommand(OrderId id, bool rest) {
    ipc::Command c{};
    c.id = id;
    c.newId = 0;
    c.price = kMid - kTick;
    c.quantity = 10;
    c.participant = rest ? 2 : 3;
    c.kind = ipc::CommandKind::New;
    c.side = rest ? Side::Buy : Side::Sell;
    c.type = OrderType::Limit;
    return c;
}

// --- T4: the outbound A/B --------------------------------------------------------------------
//
// Design A: two independent SpscRing<OutboundEvent> -- the engine calls push() on each, so it
// pays a second hot-path write per event.
// Design B: one MulticastRing<OutboundEvent, 2> -- one write, but tryClaim() gates on the MIN of
// two consumer cursors instead of one.
//
// Measured here: sustained publish throughput and the p99 cost of publishing ONE event, with
// both consumers draining concurrently, for each design. This is what T4 asks to be measured
// "with data" rather than assumed from which one sounds more like LMAX.

struct AbResult {
    double throughputPerSec;
    long long p50Ns;
    long long p99Ns;
    long long p999Ns;
};

AbResult benchDesignA(std::size_t numEvents) {
    ipc::SpscRing<ipc::OutboundEvent> ringExec;
    ipc::SpscRing<ipc::OutboundEvent> ringMarketData;
    std::atomic<bool> stop{false};

    auto drain = [&](ipc::SpscRing<ipc::OutboundEvent>& r) {
        ipc::OutboundEvent ev;
        while (!stop.load(std::memory_order_acquire)) {
            if (!r.pop(ev)) platform::cpuPause();
        }
        while (r.pop(ev)) {
        }
    };
    std::thread consumer1(drain, std::ref(ringExec));
    std::thread consumer2(drain, std::ref(ringMarketData));

    hdr_histogram* hist = nullptr;
    hdr_init(1, 10'000'000'000LL, 3, &hist);

    constexpr std::size_t kBatch = 256;
    const ipc::OutboundEvent e = ipc::tradeEvent(Trade{}, 0);

    const auto t0 = std::chrono::steady_clock::now();
    std::size_t i = 0;
    while (i < numEvents) {
        const std::size_t n = std::min(kBatch, numEvents - i);
        const auto b0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < n; ++k) {
            while (!ringExec.push(e)) platform::cpuPause();
            while (!ringMarketData.push(e)) platform::cpuPause();
        }
        const auto b1 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count();
        hdr_record_value(hist, ns / static_cast<long long>(n));
        i += n;
    }
    const auto t1 = std::chrono::steady_clock::now();

    stop.store(true, std::memory_order_release);
    consumer1.join();
    consumer2.join();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    AbResult r{
        .throughputPerSec = numEvents / secs,
        .p50Ns = static_cast<long long>(hdr_value_at_percentile(hist, 50.0)),
        .p99Ns = static_cast<long long>(hdr_value_at_percentile(hist, 99.0)),
        .p999Ns = static_cast<long long>(hdr_value_at_percentile(hist, 99.9)),
    };
    hdr_close(hist);
    return r;
}

AbResult benchDesignB(std::size_t numEvents) {
    ipc::MulticastRing<ipc::OutboundEvent, 2> ring;
    std::atomic<bool> stop{false};

    auto drain = [&](std::size_t idx) {
        const ipc::OutboundEvent* ev;
        while (!stop.load(std::memory_order_acquire)) {
            ev = ring.tryPeek(idx);
            if (ev == nullptr) {
                platform::cpuPause();
            } else {
                ring.consume(idx);
            }
        }
        while ((ev = ring.tryPeek(idx)) != nullptr) {
            ring.consume(idx);
        }
    };
    std::thread consumer1(drain, 0);
    std::thread consumer2(drain, 1);

    hdr_histogram* hist = nullptr;
    hdr_init(1, 10'000'000'000LL, 3, &hist);

    constexpr std::size_t kBatch = 256;
    const ipc::OutboundEvent e = ipc::tradeEvent(Trade{}, 0);

    const auto t0 = std::chrono::steady_clock::now();
    std::size_t i = 0;
    while (i < numEvents) {
        const std::size_t n = std::min(kBatch, numEvents - i);
        const auto b0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < n; ++k) {
            ipc::OutboundEvent* slot = nullptr;
            while ((slot = ring.tryClaim()) == nullptr) platform::cpuPause();
            *slot = e;
            ring.publish();
        }
        const auto b1 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count();
        hdr_record_value(hist, ns / static_cast<long long>(n));
        i += n;
    }
    const auto t1 = std::chrono::steady_clock::now();

    stop.store(true, std::memory_order_release);
    consumer1.join();
    consumer2.join();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    AbResult r{
        .throughputPerSec = numEvents / secs,
        .p50Ns = static_cast<long long>(hdr_value_at_percentile(hist, 50.0)),
        .p99Ns = static_cast<long long>(hdr_value_at_percentile(hist, 99.0)),
        .p999Ns = static_cast<long long>(hdr_value_at_percentile(hist, 99.9)),
    };
    hdr_close(hist);
    return r;
}

void printAbResult(const char* label, const AbResult& r) {
    std::printf(
        "  %-38s throughput %10.0f events/sec   p50 %5lld ns   p99 %5lld ns   p999 %5lld ns\n",
        label, r.throughputPerSec, r.p50Ns, r.p99Ns, r.p999Ns);
}

void runOutboundAb() {
    std::printf("\n=== T4: outbound design A/B (2 consumers, both draining concurrently) ===\n");
    constexpr std::size_t kEvents = 2'000'000;

    // Warm each design up once before the measured run, same rationale as velox_bench.cpp.
    benchDesignA(50'000);
    benchDesignB(50'000);

    const AbResult a = benchDesignA(kEvents);
    const AbResult b = benchDesignB(kEvents);

    printAbResult("A: two SpscRing, dual publish", a);
    printAbResult("B: one MulticastRing, min-gated", b);
    std::printf("======================================================================\n");
}

// --- T5: end-to-end saturation throughput + ring-transit latency -----------------------------

void runEndToEndSaturation() {
    std::printf("\n=== T5: end-to-end saturation (ring claim -> match complete) ===\n");

    ipc::SpscRing<ipc::Command, 1 << 16> in;
    // MulticastRing<OutboundEvent, 2> (Spec 005 T4's decision): both consumer cursors must be
    // drained below, standing in for the future execution-report router and market-data
    // publisher -- an undrained one would eventually gate the producer as backpressure.
    runtime::MatchingThread<1 << 16, 1 << 16>::OutRing out;
    runtime::MatchingThread<1 << 16, 1 << 16> mt(in, out, makeConfig());
    mt.start();

    // Populate the book directly through the ring so the steady-state depth exists before the
    // measured phase, exactly as velox_bench.cpp's populate() does for the in-process book.
    OrderId id = 1;
    for (int i = 1; i <= 50; ++i) {
        while (!in.push(ipc::Command{.id = id++,
                                     .newId = 0,
                                     .price = kMid - kTick * i,
                                     .quantity = 100,
                                     .participant = 1,
                                     .kind = ipc::CommandKind::New,
                                     .side = Side::Buy,
                                     .type = OrderType::Limit})) {
        }
        while (!in.push(ipc::Command{.id = id++,
                                     .newId = 0,
                                     .price = kMid + kTick * i,
                                     .quantity = 100,
                                     .participant = 1,
                                     .kind = ipc::CommandKind::New,
                                     .side = Side::Sell,
                                     .type = OrderType::Limit})) {
        }
    }

    // Two drainer threads -- one per MulticastRing consumer cursor, standing in for the future
    // execution-report router and market-data publisher -- otherwise an undrained cursor fills
    // the ring and the matching thread's publishOutbound() backpressure-spins forever, which
    // would make this benchmark measure outbound congestion, not matching throughput.
    std::atomic<bool> drainStop{false};
    auto drainConsumer = [&](std::size_t idx) {
        while (!drainStop.load(std::memory_order_acquire)) {
            if (out.tryPeek(idx) == nullptr) {
                platform::cpuPause();
            } else {
                out.consume(idx);
            }
        }
        while (out.tryPeek(idx) != nullptr) {
            out.consume(idx);
        }
    };
    std::thread drainer0([&] { drainConsumer(0); });
    std::thread drainer1([&] { drainConsumer(1); });

    // Let the population commands drain before the measured phase starts.
    while (mt.processedCount() < 100) {
        std::this_thread::yield();
    }

    constexpr std::size_t kWarmup = 200'000;
    constexpr std::size_t kSamples = 5'000'000;
    constexpr std::size_t kBatch = 256;

    hdr_histogram* hist = nullptr;
    hdr_init(1, 10'000'000'000LL, 3, &hist);

    auto pushOne = [&](std::size_t n) {
        const bool rest = (n % 2) == 0;
        while (!in.push(steadyStateCommand(id++, rest))) {
            platform::cpuPause();
        }
    };

    for (std::size_t i = 0; i < kWarmup; ++i) {
        pushOne(i);
    }
    while (mt.processedCount() < 100 + kWarmup) {
        std::this_thread::yield();
    }

    const auto t0 = std::chrono::steady_clock::now();
    std::size_t i = 0;
    while (i < kSamples) {
        const std::size_t n = std::min(kBatch, kSamples - i);
        const std::size_t startProcessed = mt.processedCount();

        const auto b0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < n; ++k, ++i) {
            pushOne(i);
        }
        // Ring-claim -> match-complete: wait for the matching thread to have dispatched every
        // command just claimed, then stop the clock. This is a per-BATCH figure divided by n,
        // same batching rationale as velox_bench.cpp (a single steady_clock read is ~41ns here,
        // coarser than one order).
        while (mt.processedCount() < startProcessed + n) {
            platform::cpuPause();
        }
        const auto b1 = std::chrono::steady_clock::now();

        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count();
        hdr_record_value(hist, ns / static_cast<long long>(n));
    }
    const auto t1 = std::chrono::steady_clock::now();

    drainStop.store(true, std::memory_order_release);
    drainer0.join();
    drainer1.join();
    mt.stop();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double throughput = static_cast<double>(kSamples) / secs;

    std::printf("  platform:           %s\n", platform::platformName());
    std::printf("  matching thread pinned: %s\n", mt.pinned() ? "yes" : "NOT PINNED");
    std::printf("  samples:            %zu (after %zu warmup, %zu population commands)\n", kSamples,
                kWarmup, std::size_t{100});
    std::printf("  outbound full-spins: %zu\n", mt.fullSpins());
    std::printf("\n");
    std::printf("  sustained throughput: %12.0f orders/sec   (gate: >= 1,000,000)\n", throughput);
    std::printf("\n");
    std::printf("  ring-transit latency (claim -> match-complete), batches of %zu:\n", kBatch);
    std::printf("    p50    %8lld ns\n",
                static_cast<long long>(hdr_value_at_percentile(hist, 50.0)));
    std::printf("    p99    %8lld ns\n",
                static_cast<long long>(hdr_value_at_percentile(hist, 99.0)));
    std::printf("    p999   %8lld ns\n",
                static_cast<long long>(hdr_value_at_percentile(hist, 99.9)));
    std::printf("    max    %8lld ns\n", static_cast<long long>(hdr_max(hist)));
    std::printf("\n");
    std::printf("  CAVEATS (must travel with these numbers):\n");
    std::printf("    - SATURATION throughput: a producer pushing as fast as it can. Not\n");
    std::printf("      rate-driven, so coordinated omission is ABSENT here, not corrected for.\n");
    std::printf("    - NO journal, NO durability. fsync-per-record cannot coexist with this\n");
    std::printf("      throughput on any real storage device; this is a no-durability figure.\n");
    std::printf("    - single inbound producer, two dummy outbound drainers standing in for\n");
    std::printf("      the real execution-report router / market-data publisher (Spec 007/008).\n");
    std::printf("======================================================================\n\n");

    hdr_close(hist);
}

}  // namespace

int main() {
    runOutboundAb();
    runEndToEndSaturation();
    return 0;
}
