// velox_auditd -- the Tier-3 audit tier binary (Spec 012, T5). Reads files off disk, in its own
// process, and writes into Postgres. It never talks to velox_gateway, the engine, or any ring --
// the seam is the filesystem, so killing this process (or Postgres) has zero effect on order
// entry, and killing velox_gateway has zero effect on this process finishing its current batch.
//
//   velox_auditd --journal=DIR --shards=1,2,3 --pg=<conninfo>
//                [--batch=1000] [--poll-ms=50] [--from-scratch]
//
// One {JournalTailer, AuditReplayer, PgWriter} triple and one thread per shard, reading
// <root>/shard-<id>/journal (the Spec 011 layout, runtime/shard.hpp). Each shard resumes from
// its own velox_audit.ingest_checkpoint row unless --from-scratch is given. Conninfo comes from
// --pg=; libpq also honours PGPASSWORD/.pgpass on its own if the conninfo omits a password --
// either way, nothing here ever logs it.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "audit/audit_replayer.hpp"
#include "audit/pg_writer.hpp"
#include "engine/order_book.hpp"
#include "sequencer/journal_tailer.hpp"

using namespace velox;

namespace {

std::atomic<bool> g_stop{false};

void onSignal(int) {
    g_stop.store(true, std::memory_order_release);
}

struct Args {
    std::string journalRoot;
    std::vector<std::uint32_t> shardIds;
    std::string conninfo;
    std::size_t batchSize = 1000;
    int pollMs = 50;
    bool fromScratch = false;
};

bool takeArg(const std::string& arg, const std::string& key, std::string& out) {
    if (arg.rfind(key, 0) != 0) {
        return false;
    }
    out = arg.substr(key.size());
    return true;
}

std::vector<std::uint32_t> parseShardIds(const std::string& csv) {
    std::vector<std::uint32_t> ids;
    std::stringstream ss(csv);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        if (!tok.empty()) {
            ids.push_back(static_cast<std::uint32_t>(std::stoul(tok)));
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
        } else if (takeArg(arg, "--shards=", tmp)) {
            a.shardIds = parseShardIds(tmp);
        } else if (takeArg(arg, "--pg=", tmp)) {
            a.conninfo = tmp;
        } else if (takeArg(arg, "--batch=", tmp)) {
            a.batchSize = static_cast<std::size_t>(std::stoul(tmp));
        } else if (takeArg(arg, "--poll-ms=", tmp)) {
            a.pollMs = std::stoi(tmp);
        } else if (arg == "--from-scratch") {
            a.fromScratch = true;
        }
    }
    return a;
}

// Must agree with velox_gateway's gatewayConfig() -- the shadow book here has to index prices
// exactly the way the live book that produced the journal did.
BookConfig auditBookConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 10000 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1u << 20;
    return cfg;
}

void flush(audit::PgWriter& writer, std::uint32_t shardId,
           std::vector<audit::OrderEventRow>& evBatch, std::vector<audit::TradeRow>& trBatch,
           const sequencer::Checkpoint& cp, std::uint64_t& totalIngested) {
    if (evBatch.empty() && trBatch.empty()) {
        return;
    }
    while (!writer.writeBatch(shardId, evBatch, trBatch, cp)) {
        if (g_stop.load(std::memory_order_acquire)) {
            return;
        }
        std::cerr << "AUDITD shard=" << shardId
                  << " Postgres write failed, retrying in 1s (this is a normal operating state, "
                     "not a crash)\n";
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    totalIngested += evBatch.size();
    evBatch.clear();
    trBatch.clear();
}

void runShard(const std::string& journalRoot, std::uint32_t shardId, const std::string& conninfo,
              std::size_t batchSize, int pollMs, bool fromScratch) {
    const std::filesystem::path journalDir =
        std::filesystem::path(journalRoot) / ("shard-" + std::to_string(shardId)) / "journal";

    audit::PgWriter writer(conninfo);
    sequencer::Checkpoint cp{};
    if (!fromScratch) {
        writer.loadCheckpoint(shardId, cp);  // false (no row / no connection yet) => start at 0
    }

    sequencer::JournalTailer tailer(journalDir, cp);
    audit::AuditReplayer replayer(shardId, auditBookConfig());

    std::vector<audit::OrderEventRow> evBatch;
    std::vector<audit::TradeRow> trBatch;
    std::uint64_t totalIngested = 0;
    auto lastReport = std::chrono::steady_clock::now();

    while (!g_stop.load(std::memory_order_acquire)) {
        const sequencer::TailResult r = tailer.next();
        if (r.status == sequencer::TailStatus::Ok) {
            replayer.apply(r, evBatch, trBatch);
            if (evBatch.size() >= batchSize) {
                flush(writer, shardId, evBatch, trBatch, tailer.checkpoint(), totalIngested);
            }
        } else if (r.status == sequencer::TailStatus::Corrupt) {
            std::cerr << "AUDITD shard=" << shardId
                      << " CORRUPT journal (sequence gap) at seq=" << r.globalSeq
                      << " -- stopping this shard's tailer\n";
            break;
        } else {  // NotYet
            flush(writer, shardId, evBatch, trBatch, tailer.checkpoint(), totalIngested);
            std::this_thread::sleep_for(std::chrono::milliseconds(pollMs));
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(5)) {
            // Lag is the health metric for a component allowed to lag arbitrarily -- reported as
            // "how far behind the last record WE have seen", since only the writer that produced
            // the journal knows the true tail position, and this process deliberately never talks
            // to it.
            std::cerr << "AUDITD shard=" << shardId
                      << " last_global_seq=" << tailer.checkpoint().lastGoodSeq
                      << " ingested=" << totalIngested << "\n";
            lastReport = now;
        }
    }
    flush(writer, shardId, evBatch, trBatch, tailer.checkpoint(), totalIngested);
    std::cerr << "AUDITD shard=" << shardId
              << " stopped, last_global_seq=" << tailer.checkpoint().lastGoodSeq << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    const Args args = parseArgs(argc, argv);
    if (args.journalRoot.empty() || args.shardIds.empty() || args.conninfo.empty()) {
        std::cerr << "usage: velox_auditd --journal=DIR --shards=1,2,3 --pg=<conninfo> "
                     "[--batch=1000] [--poll-ms=50] [--from-scratch]\n";
        return 2;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    std::vector<std::thread> threads;
    threads.reserve(args.shardIds.size());
    for (std::uint32_t shardId : args.shardIds) {
        threads.emplace_back(runShard, args.journalRoot, shardId, args.conninfo, args.batchSize,
                             args.pollMs, args.fromScratch);
    }
    for (auto& t : threads) {
        t.join();
    }
    return 0;
}
