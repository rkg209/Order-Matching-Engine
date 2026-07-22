// velox_live -- the real live/recover binary (Spec 006 T8), so /recover-test has an actual
// process to SIGKILL. Reads commands from stdin in the same textual scenario format the golden
// replay suite parses (common/scenario.hpp).
//
//   velox_live --mode=live    --journal=DIR [--snapshot-every=N] [--group-commit=N]
//   [--digest-out=FILE] velox_live --mode=recover --journal=DIR  --digest-out=FILE velox_live
//   --mode=bench   --journal=DIR                        # no journal, no durability
//
// --mode=live prints an ACK line per sequenced command ONLY AFTER fsync returns -- this is
// structural (Sequencer::submit() cannot return before JournalWriter::append() does, and append()
// cannot return before its fsync does), not a delay added for the printout's sake. That is what
// lets the SIGKILL test know exactly which commands were durable at kill time: whatever was ACKed
// on stdout WAS fsynced, full stop.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "common/scenario.hpp"
#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "platform/platform.hpp"
#include "recovery/recovery_manager.hpp"
#include "recovery/state_digest.hpp"
#include "runtime/matching_thread.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/sequencer.hpp"
#include "sequencer/snapshot_thread.hpp"

using namespace velox;

namespace {

struct Args {
    std::string mode;
    std::string journalRoot;
    long snapshotEvery = 100000;
    long groupCommit = 0;  // 0 => PerRecord (the default, honest-durability policy)
    std::string digestOut;
};

bool takeArg(const std::string& arg, const std::string& key, std::string& out) {
    if (arg.rfind(key, 0) != 0) {
        return false;
    }
    out = arg.substr(key.size());
    return true;
}

Args parseArgs(int argc, char** argv) {
    Args a;
    std::string tmp;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (takeArg(arg, "--mode=", tmp)) {
            a.mode = tmp;
        } else if (takeArg(arg, "--journal=", tmp)) {
            a.journalRoot = tmp;
        } else if (takeArg(arg, "--snapshot-every=", tmp)) {
            a.snapshotEvery = std::stol(tmp);
        } else if (takeArg(arg, "--group-commit=", tmp)) {
            a.groupCommit = std::stol(tmp);
        } else if (takeArg(arg, "--digest-out=", tmp)) {
            a.digestOut = tmp;
        }
    }
    return a;
}

// One fixed BookConfig for the whole binary -- live, recover, and bench must agree, or a
// recovered book would index prices differently than the one that produced the journal.
BookConfig liveConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 10000 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1u << 20;
    return cfg;
}

void writeDigestFile(const std::string& path, const recovery::StateDigest& d) {
    if (path.empty()) {
        return;
    }
    std::ofstream f(path);
    f << "lastSeq=" << d.lastSeq << " nextTradeId=" << d.nextTradeId
      << " restingOrders=" << d.restingOrders << " bodyCrc32=" << d.bodyCrc32 << "\n";
}

ipc::Command toIpcCommand(const common::ScenarioCommand& sc) {
    ipc::Command ic{};
    ic.id = sc.id;
    ic.newId = sc.newId;
    ic.price = sc.price;
    ic.quantity = sc.quantity;
    ic.participant = sc.participant;
    ic.side = sc.side;
    ic.type = sc.type;
    switch (sc.kind) {
        case common::ScenarioKind::Cancel:
            ic.kind = ipc::CommandKind::Cancel;
            break;
        case common::ScenarioKind::Replace:
            ic.kind = ipc::CommandKind::Replace;
            break;
        case common::ScenarioKind::New:
        case common::ScenarioKind::Market:
            ic.kind = ipc::CommandKind::New;
            break;
    }
    return ic;
}

int runRecover(const Args& args) {
    const std::filesystem::path root(args.journalRoot);
    OrderBook book(liveConfig());

    recovery::RecoveryManager mgr(root / "journal", root / "snapshots");
    const recovery::RecoveryResult r = mgr.recover(book);

    if (r.status != recovery::RecoveryStatus::Ok) {
        std::cerr << "RECOVER: journal tail ended with "
                  << (r.status == recovery::RecoveryStatus::TornTail ? "TornTail" : "Corrupt")
                  << " after seq " << r.lastSeq
                  << " (expected after a mid-stream kill; the torn/corrupt record was discarded)\n";
    }

    const recovery::StateDigest d = recovery::computeDigest(book);
    std::cout << "RECOVERED seq=" << r.lastSeq << " digest lastSeq=" << d.lastSeq
              << " nextTradeId=" << d.nextTradeId << " restingOrders=" << d.restingOrders
              << " bodyCrc32=" << d.bodyCrc32 << "\n";
    writeDigestFile(args.digestOut, d);
    return 0;
}

int runLive(const Args& args) {
    const std::filesystem::path root(args.journalRoot);
    const std::filesystem::path journalDir = root / "journal";
    const std::filesystem::path snapshotDir = root / "snapshots";
    const BookConfig cfg = liveConfig();

    recovery::RecoveryManager mgr(journalDir, snapshotDir);
    recovery::RecoveryResult rr;

    ipc::SpscRing<ipc::Command> inRing;
    runtime::MatchingThread<>::OutRing outRing;
    runtime::MatchingThread<> matching(inRing, outRing, cfg);

    // Recover BEFORE start(): the live process is not actually recoverable unless it does this
    // on every startup, not just when a human remembers to run a separate step (NFR-24).
    matching.restoreBeforeStart([&](OrderBook& b) { rr = mgr.recover(b); });

    const sequencer::FsyncPolicy policy =
        args.groupCommit > 0 ? sequencer::FsyncPolicy::Group : sequencer::FsyncPolicy::PerRecord;
    const std::size_t groupSize =
        args.groupCommit > 0 ? static_cast<std::size_t>(args.groupCommit) : 1;
    sequencer::JournalWriter journal(journalDir, 256u * 1024 * 1024, policy, groupSize);
    if (rr.hasJournalSegment) {
        // The reader stopped mid-segment (a torn tail or, in principle, a sequence gap): resume
        // there, truncating to the last known-good record. "The writer truncates the segment to
        // that offset and continues" -- there is no separate repair tool, ever.
        journal.resumeFrom(rr.resumeSegmentPath, rr.resumeOffset, rr.resumeSegmentCreatedCounter,
                           rr.lastSeq);
    }

    matching.start();

    sequencer::SnapshotThread snapshotThread(journalDir, snapshotDir, cfg);
    snapshotThread.start();

    sequencer::Sequencer<ipc::SpscRing<ipc::Command>> seqr(journal, inRing, rr.lastSeq);

    std::cerr << "LIVE recovered_seq=" << rr.lastSeq << " fsync=" << platform::fsyncMechanismName()
              << " policy="
              << (policy == sequencer::FsyncPolicy::PerRecord
                      ? std::string("PerRecord")
                      : ("Group(" + std::to_string(groupSize) + ")"))
              << "\n";

    std::string line;
    std::size_t submittedThisRun = 0;
    while (std::getline(std::cin, line)) {
        common::ScenarioCommand sc;
        if (!common::parseScenarioLine(line, sc)) {
            continue;
        }
        const ipc::Command ic = toIpcCommand(sc);

        const Seq acked = seqr.submit(ic.kind, ic);
        if (acked == 0) {
            std::cerr << "DURABILITY FAILURE -- journal append/fsync failed, aborting\n";
            snapshotThread.stop();
            matching.stop();
            return 1;
        }
        ++submittedThisRun;
        // Wait for the matching thread to have fully dispatched this command (all its outbound
        // events already published) before draining -- same technique replay_test.cpp uses.
        while (matching.processedCount() < submittedThisRun) {
            platform::cpuPause();
        }
        const ipc::OutboundEvent* ev;
        while ((ev = outRing.tryPeek(0)) != nullptr) {
            if (ev->kind == ipc::OutboundKind::TradeEvent) {
                const Trade& t = ev->payload.trade;
                std::cout << "TRADE " << t.id << " agg=" << t.aggressorId << " pass=" << t.passiveId
                          << " px=" << t.price << " qty=" << t.quantity << "\n";
            }
            outRing.consume(0);
        }
        while (outRing.tryPeek(1) != nullptr) {
            outRing.consume(1);  // second consumer slot; drained only to avoid backpressure
        }

        // Printed only now: submit() cannot return before the journal append -- including its
        // fsync -- has returned. Whatever line reaches stdout WAS durable at that moment.
        std::cout << "ACK " << acked << "\n";
        std::cout.flush();

        if (args.snapshotEvery > 0 && acked % args.snapshotEvery == 0) {
            snapshotThread.requestSnapshot(acked);
        }
    }

    snapshotThread.stop();
    matching.stop();
    return 0;
}

// journal OFF entirely: this is the 1,000,000 orders/sec headline path (bench mode), and it is a
// NO-DURABILITY number -- see spec "Conflict 2". Drives the book directly, no ring, no journal.
int runBench(const Args&) {
    OrderBook book(liveConfig());
    Trade storage[64];
    TradeBuffer trades{storage, 64, 0};

    std::string line;
    std::size_t n = 0;
    while (std::getline(std::cin, line)) {
        common::ScenarioCommand sc;
        if (!common::parseScenarioLine(line, sc)) {
            continue;
        }
        trades.clear();
        switch (sc.kind) {
            case common::ScenarioKind::New:
            case common::ScenarioKind::Market: {
                const NewOrder o{sc.id, sc.price, sc.quantity, sc.participant, sc.side, sc.type};
                book.submit(o, trades);
                break;
            }
            case common::ScenarioKind::Cancel:
                book.cancel(sc.id);
                break;
            case common::ScenarioKind::Replace: {
                const NewOrder fresh{sc.newId,       sc.price, sc.quantity,
                                     sc.participant, sc.side,  OrderType::Limit};
                book.replace(sc.id, fresh, trades);
                break;
            }
        }
        ++n;
    }
    std::cout << "BENCH processed=" << n << " (no journal, no durability)\n";
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.journalRoot.empty() && args.mode != "bench") {
        std::cerr << "usage: velox_live --mode=live|recover|bench --journal=DIR [...]\n";
        return 2;
    }
    if (args.mode == "live") return runLive(args);
    if (args.mode == "recover") return runRecover(args);
    if (args.mode == "bench") return runBench(args);
    std::cerr << "usage: velox_live --mode=live|recover|bench --journal=DIR [...]\n";
    return 2;
}
