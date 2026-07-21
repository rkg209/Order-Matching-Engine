// Spec 003: property-based invariant tests.
//
// Golden replay (tests/replay/) proves the engine is right on sequences we thought of. This
// file inverts that: it asserts I1-I8 hold after EVERY operation, across thousands of
// adversarially generated schedules, and shrinks any failure to a minimal counterexample
// reproducible from a printed seed. See .claude/plans/003-invariants-property-tests.md.

#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <string>

#include "tests/invariant/invariants.hpp"
#include "tests/invariant/schedule.hpp"
#include "tests/invariant/shrink.hpp"

using namespace velox;
using namespace velox::invariant;

namespace {

std::uint64_t envU64(const char* name, std::uint64_t def) {
    if (const char* v = std::getenv(name); v != nullptr && v[0] != '\0') {
        return std::strtoull(v, nullptr, 10);
    }
    return def;
}

// Sized so all profiles together run in ~30s in Release (see spec plan's "Note on randomness"
// / runtime budget). VELOX_SCHEDULES / VELOX_OPS / VELOX_SEED override for a long soak.
std::uint64_t baseSeed() {
    return envU64("VELOX_SEED", 0xC0FFEEULL);
}
std::size_t schedulesPerProfile() {
    return static_cast<std::size_t>(envU64("VELOX_SCHEDULES", 150));
}
std::size_t opsPerSchedule() {
    return static_cast<std::size_t>(envU64("VELOX_OPS", 150));
}

// Runs `count` generated schedules for `profile` (optionally pinned to a specific StpPolicy),
// and on the first violation, shrinks it and FAILs with the full report. Every invocation
// prints its base seed unconditionally, so a bare CI log is always enough to reproduce.
void runProperty(Profile profile, std::optional<StpPolicy> stpOverride = std::nullopt) {
    const std::uint64_t base = baseSeed();
    const std::size_t n = schedulesPerProfile();
    const std::size_t maxOps = opsPerSchedule();

    std::cout << "[ property ] profile=" << profileName(profile) << " baseSeed=" << base
              << " schedules=" << n << " opsEach=" << maxOps << std::endl;

    for (std::size_t i = 0; i < n; ++i) {
        const std::uint64_t seed = base + i;
        Schedule sched = generate(profile, seed, maxOps, stpOverride);
        RunResult r = runSchedule(sched);
        if (!r.violation) continue;

        ShrinkOutcome shrunk = shrink(sched, *r.violation);
        FAIL()
            << "Invariant " << shrunk.violation.name << " violated.\n"
            << "  profile:   " << profileName(profile) << "\n"
            << "  seed:      " << seed << " (VELOX_SEED=" << seed << " reproduces this exact"
            << " schedule)\n"
            << "  detail:    " << shrunk.violation.detail << "\n"
            << "  op index:  " << shrunk.violation.opIndex << " (in shrunk schedule)\n"
            << "  shrink:    " << shrunk.stepsUsed << " steps, " << shrunk.schedule.ops.size()
            << " ops remain\n"
            << "  --- paste below into tests/replay/scenarios/<name>.txt and bless a golden ---\n"
            << renderAsReplayScenario(shrunk.schedule);
        return;
    }
}

}  // namespace

// --- Sanity: the checker itself, against a hand-written schedule (must be trusted before it is
// pointed at random input -- step 2 of the implementation plan). -------------------------------
TEST(InvariantSanity, HandWrittenSchedule) {
    Schedule sched;
    sched.cfg.minPrice = 1 * kPriceScale;
    sched.cfg.maxPrice = 200 * kPriceScale;
    sched.cfg.tick = kPriceScale / 100;
    sched.cfg.maxOrders = 64;
    sched.cfg.stp = StpPolicy::CancelAggressor;

    auto limit = [](OrderId id, Side side, double price, Quantity qty, ParticipantId pid) {
        Op op;
        op.kind = OpKind::Submit;
        op.id = id;
        op.side = side;
        op.price = static_cast<Price>(price * kPriceScale);
        op.qty = qty;
        op.participant = pid;
        op.type = OrderType::Limit;
        return op;
    };

    sched.ops.push_back(limit(1, Side::Buy, 100.00, 10, 1));  // rests
    sched.ops.push_back(limit(2, Side::Buy, 99.00, 5, 2));    // rests, behind on price
    sched.ops.push_back(limit(3, Side::Sell, 100.00, 4, 3));  // partial fill of order 1
    sched.ops.push_back(limit(4, Side::Sell, 100.00, 6, 4));  // fully fills order 1
    Op cancel2;
    cancel2.kind = OpKind::Cancel;
    cancel2.id = 2;
    sched.ops.push_back(cancel2);                            // cancel order 2
    sched.ops.push_back(limit(5, Side::Buy, 101.00, 8, 5));  // new best bid
    Op replace5;
    replace5.kind = OpKind::Replace;
    replace5.id = 5;
    replace5.newId = 6;
    replace5.side = Side::Buy;
    replace5.price = static_cast<Price>(102.00 * kPriceScale);
    replace5.qty = 8;
    replace5.participant = 5;
    sched.ops.push_back(replace5);  // cancel-replace resets priority

    RunResult r = runSchedule(sched);
    ASSERT_FALSE(r.violation.has_value()) << (r.violation ? r.violation->detail : std::string{});
}

// --- Property tests, one per profile. -----------------------------------------------------
TEST(Property, Uniform) {
    runProperty(Profile::Uniform);
}
TEST(Property, HeavyCancel) {
    runProperty(Profile::HeavyCancel);
}
TEST(Property, SinglePrice) {
    runProperty(Profile::SinglePrice);
}
TEST(Property, AlternatingCross) {
    runProperty(Profile::AlternatingCross);
}
TEST(Property, DrainRefill) {
    runProperty(Profile::DrainRefill);
}
TEST(Property, LevelChurn) {
    runProperty(Profile::LevelChurn);
}
TEST(Property, ReplaceHeavy) {
    runProperty(Profile::ReplaceHeavy);
}
TEST(Property, TinyPool) {
    runProperty(Profile::TinyPool);
}
TEST(Property, NarrowRange) {
    runProperty(Profile::NarrowRange);
}

// StpHeavy gets one instance per StpPolicy (DoD): the policy changes which of I1's two layers
// applies and changes STP mechanics entirely, so each is a genuinely different adversarial case.
TEST(Property, StpHeavyCancelAggressor) {
    runProperty(Profile::StpHeavy, StpPolicy::CancelAggressor);
}
TEST(Property, StpHeavyCancelPassive) {
    runProperty(Profile::StpHeavy, StpPolicy::CancelPassive);
}
TEST(Property, StpHeavyCancelBoth) {
    runProperty(Profile::StpHeavy, StpPolicy::CancelBoth);
}

// Determinism closes the loop on Principle 3: the harness is random, the engine is not. Same
// seed must produce a byte-identical trade stream (digested) every time.
TEST(Property, SameSeedProducesIdenticalTradeDigest) {
    const std::uint64_t seed = baseSeed();
    Schedule sched = generate(Profile::Uniform, seed, opsPerSchedule());

    const RunResult first = runSchedule(sched);
    const RunResult second = runSchedule(sched);
    const RunResult third = runSchedule(sched);

    ASSERT_FALSE(first.violation.has_value());
    EXPECT_EQ(first.tradeDigest, second.tradeDigest);
    EXPECT_EQ(second.tradeDigest, third.tradeDigest);
}
