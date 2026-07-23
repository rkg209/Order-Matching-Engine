// velox_gateway -- the real network-facing binary (Spec 007 T5). Recovers from the journal
// exactly the way apps/velox_live.cpp does, then binds a TCP acceptor instead of reading a
// scenario file from stdin. Recovery completes before the first byte is accepted.
//
//   velox_gateway --journal=DIR --port=PORT --creds=FILE [--instrument=ID]

#include <asio.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include "engine/order_book.hpp"
#include "gateway/auth.hpp"
#include "gateway/gateway.hpp"
#include "ipc/command.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "platform/platform.hpp"
#include "protocol/message_types.hpp"
#include "recovery/recovery_manager.hpp"
#include "runtime/matching_thread.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/sequencer.hpp"
#include "sequencer/snapshot_thread.hpp"

using namespace velox;

namespace {

struct Args {
    std::string journalRoot;
    unsigned short port = 9001;
    std::string credsFile;
    protocol::InstrumentId instrumentId = 1;
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
        if (takeArg(arg, "--journal=", tmp)) {
            a.journalRoot = tmp;
        } else if (takeArg(arg, "--port=", tmp)) {
            a.port = static_cast<unsigned short>(std::stoi(tmp));
        } else if (takeArg(arg, "--creds=", tmp)) {
            a.credsFile = tmp;
        } else if (takeArg(arg, "--instrument=", tmp)) {
            a.instrumentId = static_cast<protocol::InstrumentId>(std::stoul(tmp));
        }
    }
    return a;
}

// Must agree with recover/live/bench everywhere else in the repo, or a recovered book indexes
// prices differently than the one that produced the journal.
BookConfig gatewayConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 10000 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1u << 20;
    return cfg;
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.journalRoot.empty() || args.credsFile.empty()) {
        std::cerr << "usage: velox_gateway --journal=DIR --port=PORT --creds=FILE "
                     "[--instrument=ID]\n";
        return 2;
    }

    gateway::AuthHandler auth;
    if (!auth.loadFromFile(args.credsFile)) {
        std::cerr << "failed to load credentials file: " << args.credsFile << "\n";
        return 2;
    }

    const std::filesystem::path root(args.journalRoot);
    const std::filesystem::path journalDir = root / "journal";
    const std::filesystem::path snapshotDir = root / "snapshots";
    const BookConfig cfg = gatewayConfig();

    recovery::RecoveryManager mgr(journalDir, snapshotDir);
    recovery::RecoveryResult rr;

    ipc::SpscRing<ipc::Command> inRing;
    runtime::MatchingThread<>::OutRing outRing;
    runtime::MatchingThread<> matching(inRing, outRing, cfg);

    // Recover BEFORE the acceptor binds (NFR-24): the process is not actually recoverable
    // unless this happens on every startup, and no client can reach a book that hasn't been
    // rebuilt yet.
    matching.restoreBeforeStart([&](OrderBook& b) { rr = mgr.recover(b); });
    matching.restoreDispatchSeq(rr.lastSeq);

    sequencer::JournalWriter journal(journalDir);
    if (rr.hasJournalSegment) {
        journal.resumeFrom(rr.resumeSegmentPath, rr.resumeOffset, rr.resumeSegmentCreatedCounter,
                           rr.lastSeq);
    }

    matching.start();

    sequencer::SnapshotThread snapshotThread(journalDir, snapshotDir, cfg);
    snapshotThread.start();

    sequencer::Sequencer<ipc::SpscRing<ipc::Command>> seqr(journal, inRing, rr.lastSeq);

    asio::io_context io;
    gateway::GatewayServer server(io, seqr, outRing, std::move(auth), args.instrumentId,
                                  cfg.minPrice, cfg.maxPrice);
    server.listen(args.port);
    server.startRouter();

    std::cerr << "GATEWAY listening port=" << args.port << " recovered_seq=" << rr.lastSeq
              << " fsync=" << platform::fsyncMechanismName() << "\n";

    io.run();  // blocks until the io_context is stopped (or all work completes)

    server.stopRouter();
    snapshotThread.stop();
    matching.stop();
    return 0;
}
