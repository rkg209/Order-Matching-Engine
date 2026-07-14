// Golden replay (FR-47).
//
// Feed a fixed command sequence through the engine, serialize the resulting trades, and compare
// BYTE-FOR-BYTE against a committed reference file. Not "equivalent" -- identical.
//
// This is the load-bearing correctness test of the whole project. Determinism is what makes it
// possible: the same input always produces the same output, so any divergence is exactly
// reproducible and can be pinned to a single record. It is also the mechanism that later lets
// Spec 004's latency work PROVE it changed nothing about results.

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "engine/order_book.hpp"

using namespace velox;

namespace {

namespace fs = std::filesystem;

struct Command {
    OrderId id;
    Side side;
    Price price;
    Quantity quantity;
    ParticipantId participant;
};

// Scenario format (one command per line, '#' comments ignored):
//   NEW <id> <BUY|SELL> <price> <qty> <participantId>
std::vector<Command> loadScenario(const fs::path& p) {
    std::vector<Command> cmds;
    std::ifstream in(p);
    EXPECT_TRUE(in.good()) << "cannot open scenario: " << p;

    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ss(line);
        std::string verb, sideStr;
        Command c{};
        double price = 0;
        ss >> verb >> c.id >> sideStr >> price >> c.quantity >> c.participant;
        if (verb != "NEW") continue;  // Spec 001 is limit orders only
        c.side = (sideStr == "BUY") ? Side::Buy : Side::Sell;
        c.price = static_cast<Price>(price * kPriceScale);
        cmds.push_back(c);
    }
    return cmds;
}

// The serialized form IS the contract. Keep it stable -- changing it invalidates every golden
// file, which is exactly the loud, deliberate event it should be.
std::string runScenario(const std::vector<Command>& cmds) {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;

    OrderBook book(cfg);
    std::array<Trade, 256> storage{};
    TradeBuffer buf{storage.data(), storage.size(), 0};

    std::ostringstream out;
    for (const auto& c : cmds) {
        buf.clear();
        NewOrder o{
            .id = c.id,
            .price = c.price,
            .quantity = c.quantity,
            .participant = c.participant,
            .side = c.side,
        };
        const SubmitStatus st = book.submit(o, buf);

        // If a scenario ever overflows the trade buffer we must know, not silently truncate.
        EXPECT_FALSE(buf.overflowed()) << "trade buffer overflow on order " << c.id;

        if (st != SubmitStatus::Ok) {
            out << "REJECT " << c.id << " " << static_cast<int>(st) << "\n";
            continue;
        }
        for (std::size_t i = 0; i < buf.count; ++i) {
            const Trade& t = buf.data[i];
            out << "TRADE " << t.id << " agg=" << t.aggressorId << " pass=" << t.passiveId
                << " px=" << t.price << " qty=" << t.quantity << "\n";
        }
    }

    // Final book state is part of the golden output. Trades alone are not enough: an engine
    // could produce the right trades and still corrupt the resting book, and that bug would
    // pass a trades-only comparison.
    out << "BOOK bestBid=" << book.bestBid() << " bestAsk=" << book.bestAsk()
        << " resting=" << book.restingOrders() << "\n";
    return out.str();
}

void runGolden(const std::string& name) {
    const fs::path dir = fs::path(VELOX_REPLAY_DIR);
    const fs::path scenario = dir / "scenarios" / (name + ".txt");
    const fs::path golden = dir / "golden" / (name + ".golden");

    const std::string produced = runScenario(loadScenario(scenario));

    // Regenerate mode: VELOX_BLESS=1 writes the golden instead of checking it.
    //
    // This exists because golden files have to come from somewhere -- but it is deliberately
    // opt-in and loud. A golden file blessed without a human reading it is a golden file that
    // enshrines a bug and then defends it against every future fix.
    if (const char* bless = std::getenv("VELOX_BLESS"); bless != nullptr && bless[0] == '1') {
        fs::create_directories(golden.parent_path());
        std::ofstream(golden) << produced;
        GTEST_SKIP() << "BLESSED " << golden << " -- read it before committing.";
    }

    std::ifstream gf(golden);
    ASSERT_TRUE(gf.good()) << "missing golden file: " << golden
                           << "\nGenerate with VELOX_BLESS=1, then READ IT before committing.";
    std::stringstream expected;
    expected << gf.rdbuf();

    // Byte-for-byte.
    EXPECT_EQ(produced, expected.str()) << "Golden replay diverged for scenario: " << name;
}

}  // namespace

TEST(GoldenReplay, LimitMatch) {
    runGolden("limit_match");
}
TEST(GoldenReplay, PartialFill) {
    runGolden("partial_fill");
}
TEST(GoldenReplay, FullFill) {
    runGolden("full_fill");
}
TEST(GoldenReplay, CrossingBook) {
    runGolden("crossing_book");
}
TEST(GoldenReplay, EmptyBook) {
    runGolden("empty_book");
}
TEST(GoldenReplay, FifoSamePrice) {
    runGolden("fifo_same_price");
}
TEST(GoldenReplay, PriceImprovement) {
    runGolden("price_improvement");
}

// Determinism is not incidental -- it is the property every other guarantee rests on. Replay
// recovery, golden tests, and reproducible bug reports all collapse without it, so it gets its
// own test rather than being assumed.
TEST(GoldenReplay, SameInputProducesByteIdenticalOutputAcrossRuns) {
    const fs::path scenario = fs::path(VELOX_REPLAY_DIR) / "scenarios" / "crossing_book.txt";
    const auto cmds = loadScenario(scenario);

    const std::string first = runScenario(cmds);
    const std::string second = runScenario(cmds);
    const std::string third = runScenario(cmds);

    EXPECT_EQ(first, second);
    EXPECT_EQ(second, third);
}
