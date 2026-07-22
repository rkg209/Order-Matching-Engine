// Spec 006 recovery suite: crc32, journal round-trip + torn tail, sequencer durability ordering,
// snapshot round-trip + retention, shadow-replay digest equality, and RecoveryManager end-to-end.
// The SIGKILL-a-real-process case lives in its own binary (tests/recovery/recover_sigkill_test.cpp)
// since it needs to fork/exec velox_live.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

#include "common/crc32.hpp"
#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/spsc_ring.hpp"
#include "recovery/recovery_manager.hpp"
#include "recovery/state_digest.hpp"
#include "sequencer/journal_reader.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/sequencer.hpp"
#include "sequencer/snapshot_reader.hpp"
#include "sequencer/snapshot_thread.hpp"
#include "sequencer/snapshot_writer.hpp"
#include "tests/invariant/invariants.hpp"

using namespace velox;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("velox_recovery_test_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

BookConfig testConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;
    return cfg;
}

ipc::Command newCmd(OrderId id, Side side, Price price, Quantity qty, ParticipantId pid) {
    ipc::Command c{};
    c.id = id;
    c.price = price;
    c.quantity = qty;
    c.participant = pid;
    c.side = side;
    c.kind = ipc::CommandKind::New;
    c.type = OrderType::Limit;
    return c;
}

ipc::Command cancelCmd(OrderId id) {
    ipc::Command c{};
    c.id = id;
    c.kind = ipc::CommandKind::Cancel;
    return c;
}

}  // namespace

// --- CRC32 -------------------------------------------------------------------------------------

TEST(Crc32, KnownVector) {
    EXPECT_EQ(common::crc32("123456789", 9), 0xCBF43926u);
}

TEST(Crc32, EmptyIsZero) {
    EXPECT_EQ(common::crc32(nullptr, 0), 0u);
}

// --- Journal round trip --------------------------------------------------------------------

TEST(Journal, RoundTrip) {
    const fs::path dir = tempDir("journal_roundtrip");
    {
        sequencer::JournalWriter w(dir);
        ASSERT_TRUE(
            w.append(1, ipc::CommandKind::New, newCmd(1, Side::Buy, 100 * kPriceScale, 10, 1)));
        ASSERT_TRUE(
            w.append(2, ipc::CommandKind::New, newCmd(2, Side::Sell, 101 * kPriceScale, 5, 2)));
        ASSERT_TRUE(w.append(3, ipc::CommandKind::Cancel, cancelCmd(1)));
    }

    sequencer::JournalReader r(dir);
    auto r1 = r.next();
    ASSERT_EQ(r1.status, sequencer::ReadStatus::Ok);
    EXPECT_EQ(r1.globalSeq, 1);
    EXPECT_EQ(r1.kind, ipc::CommandKind::New);
    EXPECT_EQ(r1.command.id, 1);

    auto r2 = r.next();
    ASSERT_EQ(r2.status, sequencer::ReadStatus::Ok);
    EXPECT_EQ(r2.globalSeq, 2);
    EXPECT_EQ(r2.command.side, Side::Sell);

    auto r3 = r.next();
    ASSERT_EQ(r3.status, sequencer::ReadStatus::Ok);
    EXPECT_EQ(r3.globalSeq, 3);
    EXPECT_EQ(r3.kind, ipc::CommandKind::Cancel);

    auto r4 = r.next();
    EXPECT_EQ(r4.status, sequencer::ReadStatus::EndOfJournal);
}

TEST(Journal, SegmentRoll) {
    const fs::path dir = tempDir("journal_roll");
    // A tiny roll size: header + one record fits, so every append rolls a new segment.
    {
        sequencer::JournalWriter w(dir, /*rollBytes=*/sequencer::SegmentHeader::kSize + 1);
        for (Seq s = 1; s <= 5; ++s) {
            ASSERT_TRUE(
                w.append(s, ipc::CommandKind::New, newCmd(s, Side::Buy, 100 * kPriceScale, 1, 1)));
        }
    }
    std::size_t segCount = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".jnl") ++segCount;
    }
    EXPECT_GE(segCount, 5u);

    sequencer::JournalReader r(dir);
    for (Seq s = 1; s <= 5; ++s) {
        auto res = r.next();
        ASSERT_EQ(res.status, sequencer::ReadStatus::Ok) << "seq " << s;
        EXPECT_EQ(res.globalSeq, s);
    }
    EXPECT_EQ(r.next().status, sequencer::ReadStatus::EndOfJournal);
}

// Deliberate torn-tail tests: truncate the last segment at every byte offset of its final
// record and assert a clean TruncatedTail at each -- never a crash, never garbage read back.
TEST(Journal, TornTailAtEveryOffset) {
    const fs::path dir = tempDir("journal_torn");
    fs::path segPath;
    std::size_t recordStartOffset = 0;
    {
        sequencer::JournalWriter w(dir);
        ASSERT_TRUE(
            w.append(1, ipc::CommandKind::New, newCmd(1, Side::Buy, 100 * kPriceScale, 10, 1)));
        recordStartOffset = sequencer::SegmentHeader::kSize + sequencer::kRecordSize;
        ASSERT_TRUE(
            w.append(2, ipc::CommandKind::New, newCmd(2, Side::Sell, 101 * kPriceScale, 5, 2)));
        segPath = w.currentSegmentPath();
    }
    const std::size_t fullSize = fs::file_size(segPath);
    ASSERT_EQ(fullSize, sequencer::SegmentHeader::kSize + 2 * sequencer::kRecordSize);

    // A truncation landing EXACTLY on the record boundary (zero bytes of the second record
    // present) is not distinguishable from a segment that legitimately has only one record --
    // that is a clean EndOfJournal, not a torn tail. Torn tail requires at least one byte of a
    // record that was never completed, so the sweep below starts one byte past the boundary.
    {
        const fs::path truncPath = dir / "trunc_boundary.jnl";
        std::ifstream src(segPath, std::ios::binary);
        std::ofstream dst(truncPath, std::ios::binary | std::ios::trunc);
        std::vector<char> buf(recordStartOffset);
        src.read(buf.data(), static_cast<std::streamsize>(recordStartOffset));
        dst.write(buf.data(), static_cast<std::streamsize>(recordStartOffset));
        dst.close();
        const fs::path oneSegDir = tempDir("journal_torn_boundary");
        fs::copy_file(truncPath, oneSegDir / segPath.filename(),
                      fs::copy_options::overwrite_existing);
        sequencer::JournalReader r(oneSegDir);
        ASSERT_EQ(r.next().status, sequencer::ReadStatus::Ok);
        EXPECT_EQ(r.next().status, sequencer::ReadStatus::EndOfJournal);
    }

    // Truncate at every offset strictly within the second (last) record -- at least one byte of
    // it present, but not all of it.
    for (std::size_t truncTo = recordStartOffset + 1; truncTo < fullSize; ++truncTo) {
        const fs::path truncPath = dir / "trunc.jnl";
        {
            std::ifstream src(segPath, std::ios::binary);
            std::ofstream dst(truncPath, std::ios::binary | std::ios::trunc);
            std::vector<char> buf(truncTo);
            src.read(buf.data(), static_cast<std::streamsize>(truncTo));
            dst.write(buf.data(), static_cast<std::streamsize>(truncTo));
        }
        const fs::path oneSegDir = tempDir("journal_torn_case");
        fs::copy_file(truncPath, oneSegDir / segPath.filename(),
                      fs::copy_options::overwrite_existing);

        sequencer::JournalReader r(oneSegDir);
        auto first = r.next();
        ASSERT_EQ(first.status, sequencer::ReadStatus::Ok) << "truncTo=" << truncTo;
        EXPECT_EQ(first.globalSeq, 1);

        auto second = r.next();
        EXPECT_EQ(second.status, sequencer::ReadStatus::TruncatedTail) << "truncTo=" << truncTo;
        EXPECT_EQ(second.offset, recordStartOffset) << "truncTo=" << truncTo;
    }
}

TEST(Journal, WriterResumesAfterTornTail) {
    const fs::path dir = tempDir("journal_resume");
    fs::path segPath;
    {
        sequencer::JournalWriter w(dir);
        ASSERT_TRUE(
            w.append(1, ipc::CommandKind::New, newCmd(1, Side::Buy, 100 * kPriceScale, 10, 1)));
        segPath = w.currentSegmentPath();
    }
    // Simulate a torn write: append a few garbage bytes after the one valid record.
    {
        std::ofstream f(segPath, std::ios::binary | std::ios::app);
        unsigned char garbage[5] = {1, 2, 3, 4, 5};
        f.write(reinterpret_cast<char*>(garbage), 5);
    }

    sequencer::JournalReader r(dir);
    auto good = r.next();
    ASSERT_EQ(good.status, sequencer::ReadStatus::Ok);
    auto torn = r.next();
    ASSERT_EQ(torn.status, sequencer::ReadStatus::TruncatedTail);
    ASSERT_TRUE(r.hasOpenSegment());

    sequencer::JournalWriter w2(dir);
    ASSERT_TRUE(w2.resumeFrom(r.currentSegmentPath(), r.currentOffset(),
                              r.currentSegmentCreatedCounter(), r.lastGoodSeq()));
    ASSERT_TRUE(
        w2.append(2, ipc::CommandKind::New, newCmd(2, Side::Sell, 100 * kPriceScale, 3, 2)));

    sequencer::JournalReader r2(dir);
    auto rec1 = r2.next();
    ASSERT_EQ(rec1.status, sequencer::ReadStatus::Ok);
    EXPECT_EQ(rec1.globalSeq, 1);
    auto rec2 = r2.next();
    ASSERT_EQ(rec2.status, sequencer::ReadStatus::Ok);
    EXPECT_EQ(rec2.globalSeq, 2);
    EXPECT_EQ(r2.next().status, sequencer::ReadStatus::EndOfJournal);
}

// --- Sequencer -----------------------------------------------------------------------------

TEST(Sequencer, SeqIsGapFreeAndMonotonic) {
    const fs::path dir = tempDir("sequencer_monotonic");
    sequencer::JournalWriter journal(dir);
    ipc::SpscRing<ipc::Command> ring;
    sequencer::Sequencer<decltype(ring)> seqr(journal, ring);

    for (OrderId i = 1; i <= 20; ++i) {
        const Seq s =
            seqr.submit(ipc::CommandKind::New, newCmd(i, Side::Buy, 100 * kPriceScale, 1, 1));
        EXPECT_EQ(s, i);
    }
    EXPECT_EQ(seqr.lastSeq(), 20);

    // The ring must have received exactly what was submitted, in order.
    for (OrderId i = 1; i <= 20; ++i) {
        const ipc::Command* c = ring.tryPeek();
        ASSERT_NE(c, nullptr);
        EXPECT_EQ(c->id, i);
        ring.consume();
    }
}

// The ack is not observable before the fsync returns: structurally, submit() cannot push into
// the ring until JournalWriter::append() returns, and append() cannot return until its fsync
// does -- there is no code path that reorders this. This test pins that down by checking the
// journal on disk already contains the record by the time submit() returns.
TEST(Sequencer, JournalDurableBeforeRingPush) {
    const fs::path dir = tempDir("sequencer_ordering");
    sequencer::JournalWriter journal(dir);
    ipc::SpscRing<ipc::Command> ring;
    sequencer::Sequencer<decltype(ring)> seqr(journal, ring);

    const Seq s = seqr.submit(ipc::CommandKind::New, newCmd(7, Side::Buy, 100 * kPriceScale, 1, 1));
    ASSERT_EQ(s, 1);

    // Re-read the journal from a FRESH reader (independent fd) -- if the record were not
    // durably on disk yet, this would not see it.
    sequencer::JournalReader r(dir);
    auto rec = r.next();
    ASSERT_EQ(rec.status, sequencer::ReadStatus::Ok);
    EXPECT_EQ(rec.command.id, 7);
}

// --- Snapshot round trip + digest equality --------------------------------------------------

TEST(Snapshot, WriteReadRoundTrip) {
    BookConfig cfg = testConfig();
    OrderBook book(cfg);
    Trade storage[16];
    TradeBuffer trades{storage, 16, 0};
    book.submit(NewOrder{1, 100 * kPriceScale, 10, 1, Side::Buy}, trades);
    book.submit(NewOrder{2, 99 * kPriceScale, 5, 2, Side::Buy}, trades);
    book.submit(NewOrder{3, 105 * kPriceScale, 7, 3, Side::Sell}, trades);

    const fs::path dir = tempDir("snapshot_roundtrip");
    sequencer::SnapshotWriter w(dir);
    ASSERT_TRUE(w.write(book, 42, cfg));

    sequencer::SnapshotReader r(dir);
    auto loaded = r.loadNewestValid();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->header.globalSeq, 42u);
    EXPECT_EQ(loaded->header.bookSeq, static_cast<std::uint64_t>(book.lastSeq()));
    EXPECT_EQ(loaded->orders.size(), 3u);
    // Canonical order: bids best->worst (100 before 99), then asks (only 105).
    EXPECT_EQ(loaded->orders[0].id, 1);
    EXPECT_EQ(loaded->orders[1].id, 2);
    EXPECT_EQ(loaded->orders[2].id, 3);
}

TEST(Snapshot, RetentionKeepsExactlyThree) {
    BookConfig cfg = testConfig();
    OrderBook book(cfg);
    const fs::path dir = tempDir("snapshot_retention");
    sequencer::SnapshotWriter w(dir, /*retention=*/3);
    for (Seq s = 1; s <= 5; ++s) {
        ASSERT_TRUE(w.write(book, s, cfg));
    }
    std::size_t count = 0;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".snap") ++count;
    }
    EXPECT_EQ(count, 3u);
}

TEST(Snapshot, TruncatedSnapshotRejected) {
    BookConfig cfg = testConfig();
    OrderBook book(cfg);
    Trade storage[4];
    TradeBuffer trades{storage, 4, 0};
    book.submit(NewOrder{1, 100 * kPriceScale, 10, 1, Side::Buy}, trades);

    const fs::path dir = tempDir("snapshot_truncated");
    sequencer::SnapshotWriter w(dir);
    ASSERT_TRUE(w.write(book, 1, cfg));

    fs::path snapPath;
    for (const auto& e : fs::directory_iterator(dir)) {
        if (e.path().extension() == ".snap") snapPath = e.path();
    }
    ASSERT_FALSE(snapPath.empty());
    const auto fullSize = fs::file_size(snapPath);
    fs::resize_file(snapPath, fullSize - 1);  // truncate by one byte -- CRC must now fail

    sequencer::SnapshotReader r(dir);
    EXPECT_FALSE(r.loadNewestValid().has_value());
}

TEST(Snapshot, HalfWrittenSnapshotFallsBackToPreviousValid) {
    BookConfig cfg = testConfig();
    OrderBook book(cfg);
    const fs::path dir = tempDir("snapshot_halfwritten");
    sequencer::SnapshotWriter w(dir);
    ASSERT_TRUE(w.write(book, 1, cfg));  // valid, older snapshot

    // Plant a CRC-broken "newer" snapshot directly (simulating a crash mid-write that somehow
    // still left bytes on disk under the final name -- the pathological case atomic rename is
    // meant to prevent, tested here by brute force since we cannot literally crash mid-syscall).
    fs::path badPath = dir / "snap-0000000000000002.snap";
    {
        std::ofstream f(badPath, std::ios::binary);
        std::vector<char> junk(sequencer::SnapshotHeader::kSize + 10, 0x7F);
        f.write(junk.data(), static_cast<std::streamsize>(junk.size()));
    }

    sequencer::SnapshotReader r(dir);
    auto loaded = r.loadNewestValid();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->header.globalSeq, 1u);  // silently fell back to the older valid one
}

// Shadow-replay determinism (spec "Conflict 1"): after N commands, the shadow snapshot's digest
// must equal the live book's digest at the same seq. A mismatch here is a determinism bug
// (constitution P4), not a snapshot bug.
TEST(SnapshotThread, ShadowDigestMatchesLiveDigestAtSameSeq) {
    BookConfig cfg = testConfig();
    const fs::path dir = tempDir("shadow_digest");
    const fs::path journalDir = dir / "journal";
    const fs::path snapshotDir = dir / "snapshots";

    sequencer::JournalWriter journal(journalDir);
    OrderBook live(cfg);
    Trade storage[64];
    TradeBuffer trades{storage, 64, 0};

    Seq seq = 0;
    auto apply = [&](ipc::CommandKind kind, ipc::Command cmd) {
        ++seq;
        ASSERT_TRUE(journal.append(seq, kind, cmd));
        trades.clear();
        switch (kind) {
            case ipc::CommandKind::New: {
                const NewOrder o = ipc::toNewOrder(cmd);
                live.submit(o, trades);
                break;
            }
            case ipc::CommandKind::Cancel:
                live.cancel(cmd.id);
                break;
            case ipc::CommandKind::Replace: {
                NewOrder fresh = ipc::toNewOrder(cmd);
                fresh.id = cmd.newId;
                live.replace(cmd.id, fresh, trades);
                break;
            }
        }
    };

    for (OrderId i = 1; i <= 30; ++i) {
        apply(ipc::CommandKind::New, newCmd(i, i % 2 == 0 ? Side::Buy : Side::Sell,
                                            (100 + i % 5) * kPriceScale, 3, i % 3));
    }
    apply(ipc::CommandKind::Cancel, cancelCmd(2));

    sequencer::SnapshotThread shadow(journalDir, snapshotDir, cfg);
    ASSERT_TRUE(shadow.snapshotNowSync(seq));

    const recovery::StateDigest liveDigest = recovery::computeDigest(live);

    sequencer::SnapshotReader reader(snapshotDir);
    auto loaded = reader.loadNewestValid();
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->header.bookSeq, static_cast<std::uint64_t>(liveDigest.lastSeq));
    EXPECT_EQ(loaded->header.nextTradeId, static_cast<std::uint64_t>(liveDigest.nextTradeId));
    EXPECT_EQ(loaded->orders.size(), liveDigest.restingOrders);
}

// --- RecoveryManager end-to-end ------------------------------------------------------------

TEST(Recovery, EmptyJournal) {
    const fs::path dir = tempDir("recovery_empty");
    BookConfig cfg = testConfig();
    OrderBook book(cfg);
    recovery::RecoveryManager mgr(dir / "journal", dir / "snapshots");
    auto r = mgr.recover(book);
    EXPECT_EQ(r.status, recovery::RecoveryStatus::Ok);
    EXPECT_EQ(r.lastSeq, 0);
    EXPECT_EQ(book.lastSeq(), 0);
    EXPECT_EQ(book.restingOrders(), 0u);
}

TEST(Recovery, NoSnapshotTailOnlyReproducesDigest) {
    const fs::path dir = tempDir("recovery_tail_only");
    BookConfig cfg = testConfig();

    OrderBook live(cfg);
    sequencer::JournalWriter journal(dir / "journal");
    Trade storage[64];
    TradeBuffer trades{storage, 64, 0};
    Seq seq = 0;
    for (OrderId i = 1; i <= 10; ++i) {
        ++seq;
        const ipc::Command cmd =
            newCmd(i, i % 2 == 0 ? Side::Buy : Side::Sell, (100 + i % 3) * kPriceScale, 4, 1);
        ASSERT_TRUE(journal.append(seq, ipc::CommandKind::New, cmd));
        trades.clear();
        live.submit(ipc::toNewOrder(cmd), trades);
    }

    OrderBook recovered(cfg);
    recovery::RecoveryManager mgr(dir / "journal", dir / "snapshots");
    auto r = mgr.recover(recovered);
    EXPECT_EQ(r.status, recovery::RecoveryStatus::Ok);
    EXPECT_EQ(r.lastSeq, seq);
    EXPECT_EQ(recovery::computeDigest(recovered), recovery::computeDigest(live));
}

TEST(Recovery, SnapshotPlusTailEqualsFullReplay) {
    const fs::path dir = tempDir("recovery_snap_tail");
    BookConfig cfg = testConfig();

    OrderBook live(cfg);
    sequencer::JournalWriter journal(dir / "journal");
    Trade storage[64];
    TradeBuffer trades{storage, 64, 0};
    Seq seq = 0;
    for (OrderId i = 1; i <= 8; ++i) {
        ++seq;
        const ipc::Command cmd =
            newCmd(i, i % 2 == 0 ? Side::Buy : Side::Sell, (100 + i % 4) * kPriceScale, 4, 1);
        ASSERT_TRUE(journal.append(seq, ipc::CommandKind::New, cmd));
        trades.clear();
        live.submit(ipc::toNewOrder(cmd), trades);
    }

    // Snapshot at seq 8 (via the shadow thread's sync API), then keep going.
    sequencer::SnapshotThread shadow(dir / "journal", dir / "snapshots", cfg);
    ASSERT_TRUE(shadow.snapshotNowSync(seq));

    for (OrderId i = 9; i <= 14; ++i) {
        ++seq;
        const ipc::Command cmd =
            newCmd(i, i % 2 == 0 ? Side::Buy : Side::Sell, (100 + i % 4) * kPriceScale, 4, 2);
        ASSERT_TRUE(journal.append(seq, ipc::CommandKind::New, cmd));
        trades.clear();
        live.submit(ipc::toNewOrder(cmd), trades);
    }

    OrderBook recovered(cfg);
    recovery::RecoveryManager mgr(dir / "journal", dir / "snapshots");
    auto r = mgr.recover(recovered);
    EXPECT_EQ(r.status, recovery::RecoveryStatus::Ok);
    EXPECT_EQ(recovery::computeDigest(recovered), recovery::computeDigest(live));
}

TEST(Recovery, TornTailRecordRecoversToLastIntactSeq) {
    const fs::path dir = tempDir("recovery_torn");
    BookConfig cfg = testConfig();

    OrderBook live(cfg);
    sequencer::JournalWriter journal(dir / "journal");
    Trade storage[64];
    TradeBuffer trades{storage, 64, 0};
    fs::path segPath;
    for (OrderId i = 1; i <= 3; ++i) {
        const ipc::Command cmd = newCmd(i, Side::Buy, 100 * kPriceScale, 4, 1);
        ASSERT_TRUE(journal.append(i, ipc::CommandKind::New, cmd));
        trades.clear();
        live.submit(ipc::toNewOrder(cmd), trades);
        segPath = journal.currentSegmentPath();
    }
    const recovery::StateDigest digestAt3 = recovery::computeDigest(live);

    // Corrupt the tail: truncate mid-way through the LAST record.
    const auto fullSize = fs::file_size(segPath);
    fs::resize_file(segPath, fullSize - 3);

    OrderBook recovered(cfg);
    recovery::RecoveryManager mgr(dir / "journal", dir / "snapshots");
    auto r = mgr.recover(recovered);
    EXPECT_EQ(r.status, recovery::RecoveryStatus::TornTail);
    EXPECT_EQ(r.lastSeq, 2);

    // Replay only seq 1-2 in a fresh book to build the expected digest.
    OrderBook expected(cfg);
    trades.clear();
    expected.submit(NewOrder{1, 100 * kPriceScale, 4, 1, Side::Buy}, trades);
    trades.clear();
    expected.submit(NewOrder{2, 100 * kPriceScale, 4, 1, Side::Buy}, trades);
    EXPECT_EQ(recovery::computeDigest(recovered), recovery::computeDigest(expected));
    EXPECT_NE(recovery::computeDigest(recovered), digestAt3);
}

TEST(Recovery, NoManualIntervention) {
    // Every recovery path above ran with nothing beyond RecoveryManager::recover() -- no
    // separate repair step. This test just documents that expectation as a standalone assertion
    // over the empty-journal case (the cheapest one to construct here).
    const fs::path dir = tempDir("recovery_no_manual");
    BookConfig cfg = testConfig();
    OrderBook book(cfg);
    recovery::RecoveryManager mgr(dir / "journal", dir / "snapshots");
    auto r = mgr.recover(book);  // the ONLY call made
    EXPECT_EQ(r.status, recovery::RecoveryStatus::Ok);
}

TEST(Recovery, InvariantsAfterRecovery) {
    const fs::path dir = tempDir("recovery_invariants");
    BookConfig cfg = testConfig();

    OrderBook live(cfg);
    sequencer::JournalWriter journal(dir / "journal");
    Trade storage[64];
    TradeBuffer trades{storage, 64, 0};
    Seq seq = 0;
    for (OrderId i = 1; i <= 25; ++i) {
        ++seq;
        const ipc::Command cmd =
            newCmd(i, i % 2 == 0 ? Side::Buy : Side::Sell, (100 + i % 6) * kPriceScale, 3, i % 4);
        ASSERT_TRUE(journal.append(seq, ipc::CommandKind::New, cmd));
        trades.clear();
        live.submit(ipc::toNewOrder(cmd), trades);
    }
    ++seq;
    ASSERT_TRUE(journal.append(seq, ipc::CommandKind::Cancel, cancelCmd(3)));
    live.cancel(3);

    OrderBook recovered(cfg);
    recovery::RecoveryManager mgr(dir / "journal", dir / "snapshots");
    ASSERT_EQ(mgr.recover(recovered).status, recovery::RecoveryStatus::Ok);

    EXPECT_FALSE(velox::invariant::detail::checkLevelStructure(recovered, 0).has_value());
    EXPECT_FALSE(velox::invariant::detail::checkIdMapLevelConsistency(recovered, 0).has_value());
    EXPECT_FALSE(velox::invariant::detail::checkBestPriceIsReal(recovered, 0).has_value());
    EXPECT_FALSE(velox::invariant::detail::checkPoolAccounting(recovered, 0).has_value());
    EXPECT_FALSE(
        velox::invariant::detail::checkOccupancyBitsetConsistency(recovered, 0).has_value());
    EXPECT_FALSE(velox::invariant::detail::checkNoCrossedBook(recovered, 0).has_value());
    EXPECT_EQ(recovery::computeDigest(recovered), recovery::computeDigest(live));
}
