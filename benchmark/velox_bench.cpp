// Latency harness (constitution Principle 6 -- measure from day one).
//
// Reports p50/p99/p999 for the matching call, captured with HdrHistogram_c. NEVER an average:
// the mean of a matching engine is dominated by the fast common case and hides the tail, and
// the tail is what this project is judged on.
//
// SCOPE NOTE, so nobody later mistakes what this proves:
//   This is a straight-line microbenchmark -- it calls the book directly, as fast as it can.
//   There is therefore NO coordinated-omission hazard here, because CO is a property of
//   RATE-DRIVEN measurement (the loop stalls with the system and silently fails to send the
//   orders that would have shown the tail). It is not solved here; it is simply absent.
//
//   Spec 009 introduces the rate-driven load generator, and with it the hazard. That is where
//   hdr_record_corrected_value() and the intended-schedule methodology become mandatory.
//   Do not read this file as evidence that CO has been handled.

#include <benchmark/benchmark.h>
#include <hdr/hdr_histogram.h>

#include <chrono>
#include <cstdio>

#include "engine/order_book.hpp"
#include "platform/platform.hpp"

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

// Build a book with real depth on both sides. Matching against an empty book measures the
// trivial path and tells you nothing.
void populate(OrderBook& book, OrderId& id, TradeBuffer& buf, int levelsPerSide) {
    for (int i = 1; i <= levelsPerSide; ++i) {
        buf.clear();
        NewOrder bid{
            .id = id++,
            .price = kMid - kTick * i,
            .quantity = 100,
            .participant = 1,
            .side = Side::Buy,
        };
        book.submit(bid, buf);

        buf.clear();
        NewOrder ask{
            .id = id++,
            .price = kMid + kTick * i,
            .quantity = 100,
            .participant = 1,
            .side = Side::Sell,
        };
        book.submit(ask, buf);
    }
}

// --- Google Benchmark: throughput of the matching call ------------------------------------

// STEADY STATE, and that is not a detail -- it is what makes this benchmark mean anything.
//
// The first version of this simply rested a new order every iteration and never matched any of
// them. The order pool holds ~1M orders; Google Benchmark ran 441M iterations. So after the
// first million the pool was exhausted and submit() was returning RejectedPoolExhausted
// immediately -- meaning ~99.8% of the "matching latency" being reported was actually the cost
// of the REJECT path. The benchmark was measuring the engine refusing to work.
//
// It looked entirely plausible (6 ns/op!) and it was nonsense. This is the failure mode the
// benchmark-methodology skill means by "if a number looks too good, it probably is."
//
// So: alternate rest / cross. Even iterations rest a bid below the ask book; odd iterations send
// a sell that crosses and fully consumes it. Net pool usage per pair is zero, the book stays at
// its populated depth forever, and the loop measures a real rest and a real match. The
// pool-exhaustion guard below turns any regression back into a loud failure rather than a lie.
void BM_SubmitRestingOrder(benchmark::State& state) {
    OrderBook book(makeConfig());
    Trade storage[64];
    TradeBuffer buf{storage, 64, 0};
    OrderId id = 1;
    populate(book, id, buf, 50);

    std::size_t rejects = 0;
    int i = 0;
    for (auto _ : state) {
        buf.clear();
        const bool rest = (i % 2) == 0;
        NewOrder o{
            .id = id++,
            // Both legs sit at the same price, just below the ask book. The BUY rests (nothing
            // to cross); the SELL then crosses into it and fully fills it.
            .price = kMid - kTick,
            .quantity = 10,
            .participant = 1,
            .side = rest ? Side::Buy : Side::Sell,
        };
        SubmitStatus st = book.submit(o, buf);
        benchmark::DoNotOptimize(st);
        if (st == SubmitStatus::RejectedPoolExhausted) ++rejects;
        ++i;
    }

    // A benchmark that silently degrades into measuring the reject path is worse than no
    // benchmark, because it reports a fast number for a broken engine.
    if (rejects > 0) {
        state.SkipWithError(
            "pool exhausted -- benchmark was measuring the REJECT path, "
            "not matching. The numbers from this run are invalid.");
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SubmitRestingOrder);

// An order that crosses and fills -- the path that actually does work.
//
// NOTE: this deliberately does NOT use state.PauseTiming()/ResumeTiming() to set up a resting
// order each iteration. Pause/Resume costs on the order of a microsecond per call, which is
// ~200x the operation being measured -- it would time Google Benchmark's own instrumentation
// and report that as the engine's latency. (The first version of this file did exactly that and
// reported 3745 ns; see progress_report.md [005].)
//
// Instead: rest ONE enormous sell up front, then have each iteration buy a small quantity out of
// it. Every iteration is a genuine crossing partial fill, and no setup happens inside the loop.
void BM_SubmitCrossingOrder(benchmark::State& state) {
    OrderBook book(makeConfig());
    Trade storage[64];
    TradeBuffer buf{storage, 64, 0};
    OrderId id = 1;

    constexpr Quantity kDepth = 2'000'000'000LL;
    constexpr Quantity kTake = 1;

    buf.clear();
    NewOrder deepAsk{
        .id = id++,
        .price = kMid,
        .quantity = kDepth,
        .participant = 1,
        .side = Side::Sell,
    };
    book.submit(deepAsk, buf);

    for (auto _ : state) {
        buf.clear();
        NewOrder aggressor{
            .id = id++,
            .price = kMid,
            .quantity = kTake,
            .participant = 2,
            .side = Side::Buy,
        };
        benchmark::DoNotOptimize(book.submit(aggressor, buf));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_SubmitCrossingOrder);

// --- HdrHistogram: the latency DISTRIBUTION (this is the number that matters) ---------------

// Empirically measure the clock's granularity: spin until the value CHANGES, and record the
// size of the jump. Repeat, take the smallest non-zero delta.
//
// This exists because a benchmark that cannot resolve the thing it is timing will happily
// report a beautiful number made entirely of quantization. On Apple Silicon steady_clock is
// backed by a ~24 MHz timebase (~41.67 ns/tick), which is COARSER than a single matching call.
// The harness must know that and say it, rather than printing "p50 = 0 ns" and letting a reader
// conclude the engine is infinitely fast.
long long measureClockGranularityNs() {
    long long best = 1'000'000;
    for (int trial = 0; trial < 1000; ++trial) {
        const auto t0 = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point t1;
        do {
            t1 = std::chrono::steady_clock::now();
        } while (t1 == t0);
        const auto d = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        if (d > 0 && d < best) best = d;
    }
    return best;
}

void reportLatencyDistribution() {
    constexpr std::size_t kWarmup = 100'000;
    constexpr std::size_t kSamples = 2'000'000;
    constexpr std::size_t kBatch = 64;  // see below

    const long long granularity = measureClockGranularityNs();

    hdr_histogram* hist = nullptr;       // per-call (granularity-limited on this platform)
    hdr_histogram* batchHist = nullptr;  // per-batch of kBatch calls (resolvable)
    // 1 ns .. 10 s, 3 significant figures.
    if (hdr_init(1, 10'000'000'000LL, 3, &hist) != 0 ||
        hdr_init(1, 10'000'000'000LL, 3, &batchHist) != 0) {
        std::fprintf(stderr, "hdr_init failed\n");
        return;
    }

    OrderBook book(makeConfig());
    Trade storage[64];
    TradeBuffer buf{storage, 64, 0};
    OrderId id = 1;
    populate(book, id, buf, 50);

    // Warm up to steady state: fill the pools, touch the pages, warm the caches and the branch
    // predictor. Measuring before this point measures startup, not matching.
    //
    // The warmup uses the same rest/cross alternation as the measured loop, so it does NOT grow
    // the book -- otherwise the warmup itself would eat into the pool it is supposed to be
    // warming, and the measured phase would inherit a partly-exhausted pool.
    for (std::size_t i = 0; i < kWarmup; ++i) {
        buf.clear();
        NewOrder o{
            .id = id++,
            .price = kMid - kTick,
            .quantity = 10,
            .participant = 1,
            .side = (i % 2) == 0 ? Side::Buy : Side::Sell,
        };
        book.submit(o, buf);
    }

    // steady_clock, not system_clock. This is the constitution's explicit carve-out: it is the
    // one clock permitted on the hot path, and only for exactly this -- latency capture at a
    // cycle boundary. It never influences a matching decision, so it cannot affect determinism.
    //
    // TWO histograms, because one is not honest by itself on this platform:
    //
    //   hist      per-CALL. On macOS-arm64 the clock ticks every ~41.67 ns, which is coarser
    //             than a single submit(). Most samples land in the same tick as their start and
    //             record 0. This distribution is therefore QUANTIZED, not precise -- it is kept
    //             only to expose that fact, never to be quoted as the engine's latency.
    //
    //   batchHist per-BATCH of kBatch calls. Each sample spans ~kBatch x the per-call cost, which
    //             is comfortably above the tick, so it IS resolvable. Dividing by kBatch gives a
    //             trustworthy per-order figure, and a real stall still shows up in the tail
    //             because it lands inside some batch.
    //
    // The batch figure is the one to report. The per-call one is evidence about the instrument.
    //
    // CRITICAL: the two passes below are SEPARATE. The per-call clock reads must NOT sit inside
    // the batch window. The first version of this file nested them, and since a steady_clock
    // read costs ~25 ns on this machine, two of them per iteration turned a ~6 ns operation into
    // a reported ~58 ns -- a 10x overstatement that was almost entirely my own instrumentation.
    // Google Benchmark's independent 5.91 ns/op is what exposed it. When two instruments
    // disagree by 10x, one of them is lying; find out which BEFORE publishing either.
    // (See progress_report.md [005].)

    // Both passes below use the same STEADY-STATE workload as BM_SubmitRestingOrder: alternate
    // a resting bid with a sell that crosses and consumes it, so the book never grows and the
    // pool is never exhausted. Driving 2M resting orders through a 1M pool -- as the first
    // version did -- means half the samples are the REJECT path, not matching.
    std::size_t rejects = 0;
    auto oneOrder = [&](std::size_t n) {
        buf.clear();
        NewOrder o{
            .id = id++,
            .price = kMid - kTick,
            .quantity = 10,
            .participant = 1,
            .side = (n % 2) == 0 ? Side::Buy : Side::Sell,
        };
        const SubmitStatus st = book.submit(o, buf);
        if (st == SubmitStatus::RejectedPoolExhausted) ++rejects;
        return st;
    };

    // --- PASS 1: batched. Nothing but submit() inside the timed window. ----------------------
    std::size_t i = 0;
    while (i < kSamples) {
        const std::size_t n = std::min(kBatch, kSamples - i);

        const auto b0 = std::chrono::steady_clock::now();
        for (std::size_t k = 0; k < n; ++k, ++i) {
            benchmark::DoNotOptimize(oneOrder(i));
        }
        const auto b1 = std::chrono::steady_clock::now();

        // Per-order cost within this batch: two clock reads amortized over kBatch operations,
        // so the instrumentation contributes well under a nanosecond per order.
        const auto batchNs = std::chrono::duration_cast<std::chrono::nanoseconds>(b1 - b0).count();
        hdr_record_value(batchHist, batchNs / static_cast<long long>(n));
    }

    // --- PASS 2: per-call, purely to DEMONSTRATE that this method cannot resolve the call -----
    // Kept because the quantization is worth showing, not because the numbers are usable.
    constexpr std::size_t kPerCallSamples = 200'000;
    for (std::size_t j = 0; j < kPerCallSamples; ++j) {
        const auto t0 = std::chrono::steady_clock::now();
        benchmark::DoNotOptimize(oneOrder(j));
        const auto t1 = std::chrono::steady_clock::now();
        hdr_record_value(hist,
                         std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    if (rejects > 0) {
        std::fprintf(stderr,
                     "\n*** INVALID RUN: %zu orders were rejected for pool exhaustion. The\n"
                     "*** histogram measured the REJECT path, not matching. Do not use these\n"
                     "*** numbers.\n\n",
                     rejects);
    }

    std::printf("\n");
    std::printf("=== velox latency distribution (order-to-match, matching call only) ===\n");
    std::printf("  platform:         %s\n", platform::platformName());
    std::printf("  core isolation:   %s\n",
                platform::supportsCoreIsolation() ? "available" : "NOT AVAILABLE on this platform");
    std::printf("  samples:          %zu (after %zu warmup)\n", kSamples, kWarmup);
    std::printf("  clock granularity:%5lld ns  (steady_clock, measured empirically)\n",
                granularity);
    std::printf("\n");

    std::printf("  PER-ORDER latency, measured over batches of %zu  <-- REPORT THIS\n", kBatch);
    std::printf("    p50    %8lld ns   (budget   2000 ns)\n",
                static_cast<long long>(hdr_value_at_percentile(batchHist, 50.0)));
    std::printf("    p99    %8lld ns   (budget  20000 ns)\n",
                static_cast<long long>(hdr_value_at_percentile(batchHist, 99.0)));
    std::printf("    p999   %8lld ns   (budget 100000 ns)\n",
                static_cast<long long>(hdr_value_at_percentile(batchHist, 99.9)));
    std::printf("    max    %8lld ns\n", static_cast<long long>(hdr_max(batchHist)));
    std::printf("\n");

    std::printf("  Per-CALL timing (NOT trustworthy on this platform -- shown as evidence):\n");
    std::printf("    p50    %8lld ns     p99  %8lld ns     p999 %8lld ns\n",
                static_cast<long long>(hdr_value_at_percentile(hist, 50.0)),
                static_cast<long long>(hdr_value_at_percentile(hist, 99.0)),
                static_cast<long long>(hdr_value_at_percentile(hist, 99.9)));
    std::printf("    A single submit() is FASTER THAN ONE CLOCK TICK (%lld ns) here, so these\n",
                granularity);
    std::printf("    values are quantized to multiples of the tick and a p50 of 0 means\n");
    std::printf("    'below measurement resolution', NOT 'zero nanoseconds'. Do not quote them.\n");
    std::printf("\n");

    std::printf("  CAVEATS (all of these must travel with the numbers):\n");
    std::printf("    - Matching call in ISOLATION. No journal, no gateway, no ring. Not an\n");
    std::printf("      end-to-end figure, and not comparable to one.\n");
    std::printf("    - Coordinated omission does not apply to this straight-line loop; it\n");
    std::printf("      becomes a real hazard with the rate-driven load generator in Spec 009.\n");
    std::printf("    - No core isolation on macOS-arm64, so the tail includes OS scheduling.\n");
    std::printf("======================================================================\n\n");

    hdr_close(hist);
    hdr_close(batchHist);
}

}  // namespace

int main(int argc, char** argv) {
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();

    reportLatencyDistribution();
    return 0;
}
