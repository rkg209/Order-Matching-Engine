// Spec 009 T4: "test the test". If an injected stall does not show up in the corrected p999,
// the coordinated-omission correction is not working and every number this harness produces is a
// lie -- this is the single most important check in the whole spec.
//
// Drives EnginePath (no journal, no TCP -- fastest path, so the injected stall dominates the
// distribution rather than being swamped by fsync/TCP noise) at a modest rate for ~2M samples,
// with one 50ms sleep_for() injected at the midpoint. This sleep_for lives in THIS file, never
// in engine/ -- the hot path must never see a wall-clock stall, only the generator does.
//
// Two assertions, together, are what pin the bug this test exists to catch (neither alone would):
//   1. corrected.p999 >= a bound derived from the stall -- the stall reached the tail.
//   2. naive.p999 < corrected.p999 / 10 -- the naive t_actual_send span HIDES it.
// Assertion 1 alone passes if every sample merely looks slow (a bug, not evidence of correction).
// Assertion 2 alone passes if the correction did nothing (naive and corrected being equal is
// itself the failure mode: it means the two histograms accidentally share a start timestamp).

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "loadgen/inflight.hpp"
#include "loadgen/latency_recorder.hpp"
#include "loadgen/paths.hpp"
#include "loadgen/schedule.hpp"
#include "loadgen/workload.hpp"
#include "platform/platform.hpp"

using namespace velox;
using namespace velox::loadgen;

namespace {

// Rate and sample count chosen so the run finishes in a few seconds even with the stall, while
// still comfortably exceeding the 1,000,000-sample threshold the honesty guard requires before
// trusting a p999 at all (see benchmark/velox_loadgen.cpp's p999Reported guard).
constexpr std::int64_t kIntervalNs = 5'000;  // 200,000/sec
constexpr std::size_t kWarmup = 50'000;
constexpr std::size_t kSamples = 2'000'000;
constexpr std::size_t kStallAtSample = kSamples / 2;
constexpr long kStallMs = 50;

// Derived, not a magic constant: a single stalled send delays every order queued behind it by up
// to the full stall duration relative to its own intended time, so the corrected distribution
// must show AT LEAST the stall length (minus one interval of slack) somewhere at its tail.
constexpr std::int64_t kExpectedTailFloorNs = (kStallMs * 1'000'000LL) - kIntervalNs;

}  // namespace

TEST(CoCorrection, InjectedStallShowsInCorrectedTailButNotNaive) {
    EnginePath path;

    OrderId nextId = 1;
    auto pop = populationCommands(Workload::Dense, nextId);
    for (const auto& c : pop) path.sendOne(c);
    path.waitProcessed(pop.size());

    InflightTable inflight;
    LatencyRecorder recorder;
    // See velox_loadgen.cpp: the reader's completion callback ONLY writes into the inflight
    // table -- never computes into a histogram inline -- so a very-late sample near the stall
    // can never block the reader and cascade into an avalanche of its own. Histograms are built
    // in one fast pass, after the reader has fully drained (recordRange() below).
    std::atomic<std::size_t> completed{0};

    const auto t0 = Clock::now();
    path.startReader(
        [&](OrderId id, std::int64_t completeNs) {
            inflight.recordComplete(id, completeNs);
            completed.fetch_add(1, std::memory_order_relaxed);
        },
        t0);

    SteadyStateGenerator gen(Workload::Dense, thinTopPrice());
    Schedule schedule(t0, kIntervalNs);

    auto sendIndexed = [&](std::size_t i) {
        const OrderId id = nextId++;
        const std::int64_t intendedNs = schedule.intendedNs(i);
        Schedule::waitUntil(schedule.intendedTime(i));
        const std::int64_t actualSendNs = nsSince(t0);
        inflight.recordSend(id, intendedNs, actualSendNs);
        path.sendOne(gen.next(id));
    };

    for (std::size_t i = 0; i < kWarmup; ++i) sendIndexed(i);

    const OrderId measuredFirstId = nextId;
    for (std::size_t i = 0; i < kSamples; ++i) {
        if (i == kStallAtSample) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kStallMs));
        }
        sendIndexed(kWarmup + i);
    }

    const std::size_t totalTracked = pop.size() + kWarmup + kSamples;
    const auto drainDeadline = Clock::now() + std::chrono::seconds(60);
    while (completed.load(std::memory_order_relaxed) < totalTracked &&
           Clock::now() < drainDeadline) {
        platform::cpuPause();
    }
    path.stopReader();

    ASSERT_GE(completed.load(std::memory_order_relaxed), totalTracked)
        << "reader did not drain every completion before the drain deadline";

    recordRange(recorder, inflight, measuredFirstId, kSamples, kIntervalNs);

    const long long correctedP999 = LatencyRecorder::percentile(recorder.corrected(), 99.9);
    const long long naiveP999 = LatencyRecorder::percentile(recorder.naive(), 99.9);

    EXPECT_GE(correctedP999, kExpectedTailFloorNs)
        << "the injected 50ms stall did not reach the corrected p999 -- "
        << "hdr_record_corrected_value is not doing its job";
    EXPECT_LT(naiveP999, correctedP999 / 10)
        << "the naive histogram is not hiding the stall the way an uncorrected loop would -- "
        << "naive=" << naiveP999 << " corrected=" << correctedP999
        << " (if these are close, the two histograms are wired to the same start timestamp)";
}
