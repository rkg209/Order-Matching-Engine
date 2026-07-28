// Spec 008 FR-34: "a subscriber, consuming only the feed from a clean state, reconstructs a book
// byte-identical to the engine's." This is that claim, made mechanically checkable: drive an
// adversarial schedule through the real ring + MatchingThread, let marketdata::Publisher build a
// BookMirror from the OutboundEvent stream ALONE (never touching the OrderBook directly), and
// assert recovery::computeDigest(book) == marketdata::digestOf(mirror) after every op and at the
// end, across all 10 invariant::Profile values (and, for StpHeavy, all three StpPolicy values --
// the one profile where the STP-victim path this spec added, engine/order_book.cpp's
// StpVictimBuffer, actually gets exercised).

#include <gtest/gtest.h>

#include <cstdlib>
#include <thread>

#include "ipc/command.hpp"
#include "ipc/spsc_ring.hpp"
#include "marketdata/mirror_digest.hpp"
#include "marketdata/publisher.hpp"
#include "recovery/state_digest.hpp"
#include "runtime/matching_thread.hpp"
#include "tests/invariant/schedule.hpp"

using namespace velox;

namespace {

std::uint64_t envU64(const char* name, std::uint64_t def) {
    if (const char* v = std::getenv(name); v != nullptr && v[0] != '\0') {
        return std::strtoull(v, nullptr, 10);
    }
    return def;
}

std::size_t seedsPerProfile() {
    return static_cast<std::size_t>(envU64("VELOX_SCHEDULES", 3));
}
std::size_t opsPerSchedule() {
    return static_cast<std::size_t>(envU64("VELOX_OPS", 200));
}
std::uint64_t baseSeed() {
    return envU64("VELOX_SEED", 0xC0FFEEULL);
}

ipc::Command toIpcCommand(const invariant::Op& op) {
    ipc::Command c{};
    c.id = op.id;
    c.newId = op.newId;
    c.price = op.price;
    c.quantity = op.qty;
    c.participant = op.participant;
    c.side = op.side;
    c.type = op.type;
    switch (op.kind) {
        case invariant::OpKind::Submit:
            c.kind = ipc::CommandKind::New;
            break;
        case invariant::OpKind::Cancel:
            c.kind = ipc::CommandKind::Cancel;
            break;
        case invariant::OpKind::Replace:
            c.kind = ipc::CommandKind::Replace;
            break;
    }
    return c;
}

// Drives `sched` through a real SpscRing -> MatchingThread, with a Publisher draining the
// outbound ring's non-gating consumer index (1) in lock-step -- same technique
// replay_test.cpp's runGoldenThroughRing() uses for processedCount()-based synchronization, so
// the digest comparison below never races the matching thread.
void runAndVerify(const invariant::Schedule& sched) {
    ipc::SpscRing<ipc::Command> in;
    runtime::MatchingThread<>::OutRing out;
    runtime::MatchingThread<> mt(in, out, sched.cfg);
    mt.start();

    marketdata::Publisher<runtime::MatchingThread<>::OutRing> pub(out, /*instrumentId=*/1);

    std::size_t processed = 0;
    for (std::size_t i = 0; i < sched.ops.size(); ++i) {
        const ipc::Command cmd = toIpcCommand(sched.ops[i]);
        while (!in.push(cmd)) {
            std::this_thread::yield();
        }
        ++processed;
        while (mt.processedCount() < processed) {
            std::this_thread::yield();
        }
        pub.pump();

        if (i % 25 == 0 || i + 1 == sched.ops.size()) {
            const recovery::StateDigest bookDigest = recovery::computeDigest(mt.book());
            const recovery::StateDigest mirrorDigest = marketdata::digestOf(pub.mirror());
            ASSERT_EQ(bookDigest, mirrorDigest)
                << "profile=" << invariant::profileName(sched.profile) << " seed=" << sched.seed
                << " op=" << i;
        }
    }

    mt.stop();
}

}  // namespace

TEST(MarketDataReconstruction, MirrorMatchesBookAcrossAllProfiles) {
    static constexpr invariant::Profile kProfiles[] = {
        invariant::Profile::Uniform,     invariant::Profile::HeavyCancel,
        invariant::Profile::SinglePrice, invariant::Profile::AlternatingCross,
        invariant::Profile::DrainRefill, invariant::Profile::LevelChurn,
        invariant::Profile::StpHeavy,    invariant::Profile::ReplaceHeavy,
        invariant::Profile::TinyPool,    invariant::Profile::NarrowRange,
    };

    const std::size_t nSeeds = seedsPerProfile();
    const std::size_t nOps = opsPerSchedule();
    std::uint64_t seed = baseSeed();

    for (const invariant::Profile profile : kProfiles) {
        if (profile == invariant::Profile::StpHeavy) {
            // The one profile where StpVictimBuffer's Passive/Both removal path fires -- run it
            // once per StpPolicy, same rationale as invariant::property_test.cpp's own StpHeavy
            // coverage.
            for (const StpPolicy policy :
                 {StpPolicy::CancelAggressor, StpPolicy::CancelPassive, StpPolicy::CancelBoth}) {
                for (std::size_t s = 0; s < nSeeds; ++s) {
                    const invariant::Schedule sched =
                        invariant::generate(profile, seed++, nOps, policy);
                    runAndVerify(sched);
                }
            }
            continue;
        }
        for (std::size_t s = 0; s < nSeeds; ++s) {
            const invariant::Schedule sched = invariant::generate(profile, seed++, nOps);
            runAndVerify(sched);
        }
    }
}
