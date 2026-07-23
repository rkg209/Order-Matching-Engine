// Spec 007 DoD 6: a full ring is backpressure, never a dropped order.
//
// Scope note: this exercises the mechanism at the Sequencer::trySubmit() level (a tiny-capacity
// ring, no consumer draining it) rather than through a live socket + ClientSession, because the
// wire-level backpressure path (ClientSession::submitOrPend / GatewayServer::retryAllPending,
// gateway/session.cpp) is a thin wrapper around exactly this call -- what has to be proven is
// that RingFull never loses the command and that retrying after drain delivers it exactly once.
// That invariant is proven here directly and deterministically, without a socket's added
// scheduling noise.

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "ipc/command.hpp"
#include "ipc/spsc_ring.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/sequencer.hpp"

using namespace velox;
using namespace velox::sequencer;

namespace {

std::filesystem::path tempDir() {
    auto p = std::filesystem::temp_directory_path() / "velox_backpressure_test";
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

ipc::Command makeNew(OrderId id) {
    ipc::Command c{};
    c.id = id;
    c.price = 100 * kPriceScale;
    c.quantity = 1;
    c.participant = 1;
    c.kind = ipc::CommandKind::New;
    c.side = Side::Buy;
    c.type = OrderType::Limit;
    return c;
}

}  // namespace

TEST(Backpressure, RingFullNeverDropsAndRetryDeliversExactlyOnce) {
    using TinyRing = ipc::SpscRing<ipc::Command, 4>;  // deliberately tiny
    TinyRing ring;
    JournalWriter journal(tempDir());
    Sequencer<TinyRing> seqr(journal, ring);

    // Push past capacity WITHOUT draining -- the ring holds at most 4.
    int sequenced = 0;
    int ringFull = 0;
    std::vector<OrderId> pendingIds;
    for (OrderId id = 1; id <= 10; ++id) {
        const TrySubmitResult r = seqr.trySubmit(ipc::CommandKind::New, makeNew(id));
        if (r.outcome == TrySubmitResult::Outcome::Sequenced) {
            ++sequenced;
        } else {
            ASSERT_EQ(r.outcome, TrySubmitResult::Outcome::RingFull);
            ++ringFull;
            pendingIds.push_back(id);
        }
    }
    EXPECT_EQ(sequenced, 4);       // exactly the ring's capacity got in
    EXPECT_EQ(ringFull, 6);        // the rest were refused, not silently accepted
    EXPECT_EQ(seqr.lastSeq(), 4);  // no gap: the journal only advanced for what was truly sequenced

    // Retry the pending ones, interleaved with draining -- exactly what GatewayServer's router
    // does: drain one slot (as the matching thread would), then give a backpressured session a
    // chance to retry. The ring only ever holds 4 at a time, so delivering all 6 pending
    // commands requires repeated drain/retry rounds, not one flat pass.
    int delivered = 0;
    ipc::Command tmp;
    for (OrderId id : pendingIds) {
        TrySubmitResult r = seqr.trySubmit(ipc::CommandKind::New, makeNew(id));
        while (r.outcome == TrySubmitResult::Outcome::RingFull) {
            ASSERT_TRUE(ring.pop(tmp)) << "ring reported full but had nothing to drain";
            r = seqr.trySubmit(ipc::CommandKind::New, makeNew(id));
        }
        EXPECT_EQ(r.outcome, TrySubmitResult::Outcome::Sequenced);
        if (r.outcome == TrySubmitResult::Outcome::Sequenced) ++delivered;
    }
    EXPECT_EQ(delivered, static_cast<int>(pendingIds.size()));
    // Count in == count out: every one of the 10 submitted commands is now durably sequenced,
    // exactly once, with no gap in the sequence.
    EXPECT_EQ(seqr.lastSeq(), 10);
}
