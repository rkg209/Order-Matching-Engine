// Spec 012 T7.2: the audit replayer's derived trades must be byte-identical to the trades the
// live engine actually produced -- not "equivalent", identical. This reuses the existing golden
// corpus (tests/replay/scenarios + golden) rather than minting a parallel one, the same
// precedent tests/shard/determinism_test.cpp follows: feed the scenario through a JournalWriter
// (as the real sequencer would), tail it with JournalTailer + AuditReplayer exactly as
// velox_auditd does, and compare the derived TRADE lines against the TRADE lines already
// committed in the golden file. No Postgres needed -- this only proves the shadow-replay
// projection is faithful, never touches libpq.

#include <gtest/gtest.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "audit/audit_replayer.hpp"
#include "common/scenario.hpp"
#include "sequencer/journal_tailer.hpp"
#include "sequencer/journal_writer.hpp"

using namespace velox;

namespace {

namespace fs = std::filesystem;

BookConfig scenarioConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;
    return cfg;
}

ipc::Command toCommand(const common::ScenarioCommand& c) {
    ipc::Command cmd{};
    cmd.id = c.id;
    cmd.newId = c.newId;
    cmd.price = c.price;
    cmd.quantity = c.quantity;
    cmd.participant = c.participant;
    cmd.side = c.side;
    cmd.type = c.type;
    switch (c.kind) {
        case common::ScenarioKind::New:
        case common::ScenarioKind::Market:
            cmd.kind = ipc::CommandKind::New;
            break;
        case common::ScenarioKind::Cancel:
            cmd.kind = ipc::CommandKind::Cancel;
            break;
        case common::ScenarioKind::Replace:
            cmd.kind = ipc::CommandKind::Replace;
            break;
    }
    return cmd;
}

// Extracts just the "TRADE ..." lines from a golden file, in order -- what the audit replayer
// can actually be compared against (it does not reconstruct REJECT/CANCEL/BOOK lines, only the
// commands it saw and the trades they produced).
std::vector<std::string> extractTradeLines(const fs::path& golden) {
    std::ifstream gf(golden);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(gf, line)) {
        if (line.rfind("TRADE ", 0) == 0) {
            lines.push_back(line);
        }
    }
    return lines;
}

}  // namespace

TEST(AuditReplayerDeterminism, MatchesGoldenTrades) {
    const fs::path dir = fs::path(VELOX_REPLAY_DIR);
    const fs::path scenarioPath = dir / "scenarios" / "crossing_book.txt";
    const fs::path goldenPath = dir / "golden" / "crossing_book.golden";

    std::ifstream in(scenarioPath);
    ASSERT_TRUE(in.good()) << "cannot open scenario: " << scenarioPath;
    const std::vector<common::ScenarioCommand> cmds = common::loadScenarioStream(in);

    const fs::path tmpDir = fs::temp_directory_path() /
                            ("velox_audit_replayer_determinism_test_" + std::to_string(::getpid()));
    fs::remove_all(tmpDir);
    const fs::path journalDir = tmpDir / "journal";

    {
        sequencer::JournalWriter writer(journalDir);
        Seq seq = 0;
        for (const auto& c : cmds) {
            const ipc::Command cmd = toCommand(c);
            ASSERT_TRUE(writer.append(++seq, cmd.kind, cmd));
        }
    }

    sequencer::JournalTailer tailer(journalDir, sequencer::Checkpoint{});
    audit::AuditReplayer replayer(/*shardId=*/1, scenarioConfig());
    std::vector<audit::OrderEventRow> orderEvents;
    std::vector<audit::TradeRow> trades;

    for (;;) {
        const sequencer::TailResult r = tailer.next();
        if (r.status == sequencer::TailStatus::NotYet) {
            break;  // fully caught up to a sealed journal -- nothing left to tail
        }
        ASSERT_EQ(r.status, sequencer::TailStatus::Ok);
        replayer.apply(r, orderEvents, trades);
    }

    std::vector<std::string> produced;
    produced.reserve(trades.size());
    for (const audit::TradeRow& t : trades) {
        std::ostringstream line;
        line << "TRADE " << t.tradeId << " agg=" << t.aggressorOrderId
             << " pass=" << t.passiveOrderId << " px=" << t.price << " qty=" << t.quantity;
        produced.push_back(line.str());
    }

    const std::vector<std::string> expected = extractTradeLines(goldenPath);
    ASSERT_FALSE(expected.empty()) << "golden file has no TRADE lines: " << goldenPath;
    EXPECT_EQ(produced, expected);

    fs::remove_all(tmpDir);
}
