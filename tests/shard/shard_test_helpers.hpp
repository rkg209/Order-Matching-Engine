#pragma once

// Shared helpers for tests/shard/*: driving a scenario (common/scenario.hpp -- the same format
// tests/replay/replay_test.cpp uses) through a real runtime::Shard, and a temp-dir factory. Not
// a golden-file serializer of its own -- determinism_test.cpp reuses tests/replay's exact text
// format and golden corpus, so a shard's output can be compared byte-for-byte against the
// existing committed reference.

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "common/scenario.hpp"
#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/outbound_event.hpp"
#include "runtime/shard.hpp"

namespace velox::shardtest {

inline std::filesystem::path makeShardTestDir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("velox_shard_test_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

// Same bounds tests/replay/replay_test.cpp uses -- shard determinism is only meaningful compared
// against the SAME golden corpus, which was generated under this config.
inline BookConfig replayCompatibleConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;
    return cfg;
}

// Room for the isolation test's jam (tests/shard/isolation_test.cpp): it deliberately floods one
// shard's ring pair (65536 in + 65536 out, runtime::MatchingThread<>'s default capacities) well
// past both, all resting (never trading, so the pool never frees anything mid-run).
inline BookConfig isolationTestConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 2000 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1u << 18;
    return cfg;
}

// Mirrors tests/replay/replay_test.cpp's runScenarioThroughRing() text format exactly (TRADE/
// REJECT/CANCEL/REPLACE lines + a final BOOK line), but driven through a runtime::Shard instead
// of a bare MatchingThread -- so this exercises the shard's OWN ring/journal/sequencer wiring,
// not just the engine underneath it.
inline std::string driveScenarioThroughShard(runtime::Shard& shard,
                                             const std::vector<common::ScenarioCommand>& cmds) {
    using Kind = common::ScenarioKind;
    std::ostringstream text;

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const common::ScenarioCommand& c = cmds[i];

        ipc::Command ic{};
        ic.id = c.id;
        ic.newId = c.newId;
        ic.price = c.price;
        ic.quantity = c.quantity;
        ic.participant = c.participant;
        ic.side = c.side;
        ic.type = c.type;
        switch (c.kind) {
            case Kind::New:
            case Kind::Market:
                ic.kind = ipc::CommandKind::New;
                break;
            case Kind::Cancel:
                ic.kind = ipc::CommandKind::Cancel;
                break;
            case Kind::Replace:
                ic.kind = ipc::CommandKind::Replace;
                break;
        }

        while (!shard.inRing().push(ic)) {
            std::this_thread::yield();
        }
        const std::size_t target =
            shard.matching().processedCount() > i ? shard.matching().processedCount() : i + 1;
        while (shard.matching().processedCount() < target) {
            std::this_thread::yield();
        }

        std::vector<Trade> trades;
        SubmitStatus status = SubmitStatus::Ok;
        bool hasStatus = false;

        const ipc::OutboundEvent* ev;
        while ((ev = shard.outRing().tryPeek(0)) != nullptr) {
            if (ev->kind == ipc::OutboundKind::TradeEvent) {
                trades.push_back(ev->payload.trade);
            } else if (ev->kind == ipc::OutboundKind::StatusEvent) {
                status = ev->payload.statusChange.status;
                hasStatus = true;
            }
            shard.outRing().consume(0);
        }

        for (const Trade& t : trades) {
            text << "TRADE " << t.id << " agg=" << t.aggressorId << " pass=" << t.passiveId
                 << " px=" << t.price << " qty=" << t.quantity << "\n";
        }

        switch (c.kind) {
            case Kind::New:
            case Kind::Market:
                if (hasStatus) {
                    text << "REJECT " << c.id << " " << static_cast<int>(status) << "\n";
                }
                break;
            case Kind::Cancel:
                if (status == SubmitStatus::Ok) {
                    text << "CANCEL " << c.id << " OK\n";
                } else {
                    text << "CANCEL " << c.id << " REJECT " << static_cast<int>(status) << "\n";
                }
                break;
            case Kind::Replace:
                if (status == SubmitStatus::Ok) {
                    text << "REPLACE " << c.id << " " << c.newId << " OK\n";
                } else {
                    text << "REPLACE " << c.id << " " << c.newId << " REJECT "
                         << static_cast<int>(status) << "\n";
                }
                break;
        }
    }

    text << "BOOK bestBid=" << shard.matching().book().bestBid()
         << " bestAsk=" << shard.matching().book().bestAsk()
         << " resting=" << shard.matching().book().restingOrders() << "\n";
    return text.str();
}

}  // namespace velox::shardtest
