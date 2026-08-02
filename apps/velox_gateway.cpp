// velox_gateway -- the real network-facing binary (Spec 007 T5; sharded per Spec 011 T6).
// Recovers every configured shard from its OWN journal exactly the way apps/velox_live.cpp
// recovers its single one, all BEFORE the acceptor binds, then routes each order to the right
// shard by instrumentId.
//
//   velox_gateway --journal=DIR --port=PORT --creds=FILE [--instruments=1,2,3] [--md-port=PORT]
//
// Journal layout is a BREAKING CHANGE from the pre-Spec-011 single-instrument gateway:
// <root>/journal became <root>/shard-<id>/journal (see runtime/shard.hpp). An existing
// single-instrument deployment's journal must be moved to <root>/shard-1/ by hand -- recovery
// from the old flat layout is deliberately NOT silently supported; a startup that quietly finds
// no journal where one used to exist is exactly the failure mode NFR-24 exists to make
// impossible. apps/velox_live.cpp stays on the flat layout untouched (it stays
// single-instrument), so recover_sigkill_test.cpp, viz/replay_determinism_test.cpp and
// --replay-journal= are unaffected by this change.
//
// --md-port (Spec 008) starts ONE market-data feed on its own acceptor, shared by every shard's
// own marketdata::Publisher (each stamps its own instrumentId, Spec 011 T5) -- draining the
// outbound ring's non-gating consumer index (1) is off the order-entry path entirely, by
// construction (GatingMask), for every shard independently.

#include <asio.hpp>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/order_book.hpp"
#include "gateway/auth.hpp"
#include "gateway/gateway.hpp"
#include "marketdata/feed_server.hpp"
#include "marketdata/publisher.hpp"
#include "marketdata/snapshot_source.hpp"
#include "platform/platform.hpp"
#include "protocol/message_types.hpp"
#include "recovery/recovery_manager.hpp"
#include "runtime/shard.hpp"

using namespace velox;

namespace {

struct Args {
    std::string journalRoot;
    unsigned short port = 9001;
    std::string credsFile;
    std::vector<protocol::InstrumentId> instrumentIds = {1};
    unsigned short mdPort = 0;  // 0 == disabled (opt-in, not every deployment wants the feed)
};

bool takeArg(const std::string& arg, const std::string& key, std::string& out) {
    if (arg.rfind(key, 0) != 0) {
        return false;
    }
    out = arg.substr(key.size());
    return true;
}

std::vector<protocol::InstrumentId> parseInstrumentIds(const std::string& csv) {
    std::vector<protocol::InstrumentId> ids;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) {
            ids.push_back(static_cast<protocol::InstrumentId>(std::stoul(tok)));
        }
    }
    return ids;
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
        } else if (takeArg(arg, "--instruments=", tmp)) {
            a.instrumentIds = parseInstrumentIds(tmp);
        } else if (takeArg(arg, "--instrument=", tmp)) {
            // Single-instrument spelling stays valid (plan T6): one id is just N=1.
            a.instrumentIds = {static_cast<protocol::InstrumentId>(std::stoul(tmp))};
        } else if (takeArg(arg, "--md-port=", tmp)) {
            a.mdPort = static_cast<unsigned short>(std::stoi(tmp));
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
    if (args.journalRoot.empty() || args.credsFile.empty() || args.instrumentIds.empty()) {
        std::cerr << "usage: velox_gateway --journal=DIR --port=PORT --creds=FILE "
                     "[--instruments=1,2,3] [--md-port=PORT]\n";
        return 2;
    }
    if (args.instrumentIds.size() > runtime::kMaxShards) {
        std::cerr << "velox_gateway: too many instruments (" << args.instrumentIds.size()
                  << " > kMaxShards=" << runtime::kMaxShards << ")\n";
        return 2;
    }

    gateway::AuthHandler auth;
    if (!auth.loadFromFile(args.credsFile)) {
        std::cerr << "failed to load credentials file: " << args.credsFile << "\n";
        return 2;
    }

    const std::filesystem::path root(args.journalRoot);
    const BookConfig cfg = gatewayConfig();

    // Build every shard, one per instrument, shard i pinned to core i (platform::pinThreadToCpu
    // reports honestly that this never actually pins on macOS-arm64 -- runtime/shard.hpp).
    runtime::ShardSet shards;
    for (std::size_t i = 0; i < args.instrumentIds.size(); ++i) {
        shards.addShard(args.instrumentIds[i], cfg, root, static_cast<int>(i));
    }

    // Recover BEFORE the acceptor binds (NFR-24), every shard from its own <root>/shard-<id>/
    // journal: the process is not actually recoverable unless this happens on every startup, and
    // no client can reach a book that hasn't been rebuilt yet.
    shards.recoverAndStartAll();
    for (std::size_t i = 0; i < shards.size(); ++i) {
        std::cerr << "GATEWAY shard instrument=" << shards[i].instrumentId()
                  << " journal=" << shards[i].journalDir().string()
                  << " recovered_seq=" << shards[i].recoveredSeq() << "\n";
    }

    asio::io_context io;
    gateway::GatewayServer server(io, shards, std::move(auth), cfg.minPrice, cfg.maxPrice);
    server.listen(args.port);

    // Spec 008/011: opt-in market-data feed, sharing this process's io_context for its
    // acceptor/broadcast (one demo binary end-to-end for Spec 010) but ONE marketdata::Publisher
    // PER SHARD, each with its own pump thread, all fanning into the SAME FeedServer/md port --
    // FeedServer::send() dispatches onto the io thread (feed_server.hpp), so N publisher threads
    // sharing it needs no new synchronization.
    //
    // Each shard's snapshot source rebuilds from ITS OWN JOURNAL (RecoveryManager), never from
    // the live matching thread's OrderBook directly: that OrderBook is mutated by its shard's
    // matching thread with no synchronization for readers, by design (constitution P2, single
    // writer) -- reading it from this thread would be a data race.
    std::unique_ptr<marketdata::FeedServer> feedServer;
    std::vector<std::unique_ptr<marketdata::Publisher<runtime::Shard::OutRing>>> publishers;
    std::vector<std::thread> publisherThreads;
    std::atomic<bool> publisherStop{false};

    if (args.mdPort != 0) {
        feedServer = std::make_unique<marketdata::FeedServer>(io);
        // Snapshot burst for a NEW subscriber replays every shard's book, one
        // SnapshotStart/L3Order.../SnapshotEnd group per instrument -- a subscriber that only
        // cares about one instrument (velox_viz --instrument=ID) simply ignores the others.
        feedServer->setSnapshotBurst([&](marketdata::Sink& sink) {
            for (std::size_t i = 0; i < shards.size(); ++i) {
                OrderBook shadow(shards[i].config());
                recovery::RecoveryManager(shards[i].journalDir(), shards[i].snapshotDir())
                    .recover(shadow);
                marketdata::BookMirror mirror;
                marketdata::loadMirrorFromBook(shadow, mirror);
                marketdata::emitSnapshotBurst(sink, shards[i].instrumentId(), mirror);
            }
        });
        feedServer->listen(args.mdPort);

        for (std::size_t i = 0; i < shards.size(); ++i) {
            runtime::Shard& shard = shards[i];
            auto snapshotSource = [&shard](marketdata::BookMirror& mirror) {
                OrderBook shadow(shard.config());
                recovery::RecoveryManager(shard.journalDir(), shard.snapshotDir()).recover(shadow);
                marketdata::loadMirrorFromBook(shadow, mirror);
            };

            auto publisher = std::make_unique<marketdata::Publisher<runtime::Shard::OutRing>>(
                shard.outRing(), shard.instrumentId());
            publisher->addSink(feedServer.get());
            publisher->setSnapshotSource(snapshotSource);

            marketdata::Publisher<runtime::Shard::OutRing>* pubPtr = publisher.get();
            publishers.push_back(std::move(publisher));
            publisherThreads.emplace_back([pubPtr, &publisherStop] {
                while (!publisherStop.load(std::memory_order_acquire)) {
                    if (pubPtr->pump() == 0) {
                        platform::cpuPause();
                    }
                }
            });
        }
    }
    server.startRouter();

    std::cerr << "GATEWAY listening port=" << args.port << " shards=" << shards.size()
              << " fsync=" << platform::fsyncMechanismName();
    if (args.mdPort != 0) {
        std::cerr << " md_port=" << args.mdPort;
    }
    std::cerr << "\n";

    io.run();  // blocks until the io_context is stopped (or all work completes)

    publisherStop.store(true, std::memory_order_release);
    for (auto& t : publisherThreads) {
        if (t.joinable()) t.join();
    }
    server.stopRouter();
    shards.stopAll();
    return 0;
}
