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
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "runtime/matching_thread.hpp"

using namespace velox;

namespace {

namespace fs = std::filesystem;

enum class Kind { New, Market, Cancel, Replace };

struct Command {
    Kind kind = Kind::New;
    OrderId id;         // NEW/MARKET/CANCEL: the order id. REPLACE: the OLD id.
    OrderId newId = 0;  // REPLACE only.
    Side side = Side::Buy;
    Price price = 0;
    Quantity quantity = 0;
    ParticipantId participant = 0;
    OrderType type = OrderType::Limit;
};

// Scenario format (one command per line, '#' comments ignored). Spec 001 wrote only the bare
// NEW form; Spec 002 adds the rest. The parser `continue`s on an unrecognized verb and reads
// the optional trailing token with `ss >> tok` (fails-and-clears harmlessly), so every Spec 001
// scenario file still parses identically today.
//
//   NEW     <id> <BUY|SELL> <price> <qty> <pid> [IOC|FOK]
//   MARKET  <id> <BUY|SELL> <qty> <pid>
//   CANCEL  <id>
//   REPLACE <oldId> <newId> <BUY|SELL> <price> <qty> <pid>
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

        ss >> verb;
        if (verb == "NEW") {
            ss >> c.id >> sideStr >> price >> c.quantity >> c.participant;
            c.side = (sideStr == "BUY") ? Side::Buy : Side::Sell;
            c.price = static_cast<Price>(price * kPriceScale);
            std::string tok;
            if (ss >> tok) {
                if (tok == "IOC")
                    c.type = OrderType::Ioc;
                else if (tok == "FOK")
                    c.type = OrderType::Fok;
            }
            c.kind = Kind::New;
        } else if (verb == "MARKET") {
            ss >> c.id >> sideStr >> c.quantity >> c.participant;
            c.side = (sideStr == "BUY") ? Side::Buy : Side::Sell;
            c.type = OrderType::Market;
            c.kind = Kind::Market;
        } else if (verb == "CANCEL") {
            ss >> c.id;
            c.kind = Kind::Cancel;
        } else if (verb == "REPLACE") {
            ss >> c.id >> c.newId >> sideStr >> price >> c.quantity >> c.participant;
            c.side = (sideStr == "BUY") ? Side::Buy : Side::Sell;
            c.price = static_cast<Price>(price * kPriceScale);
            c.kind = Kind::Replace;
        } else {
            continue;  // unrecognized verb -- forward-compatible with older scenario files
        }
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

    auto emitTrades = [&](std::ostringstream& out) {
        for (std::size_t i = 0; i < buf.count; ++i) {
            const Trade& t = buf.data[i];
            out << "TRADE " << t.id << " agg=" << t.aggressorId << " pass=" << t.passiveId
                << " px=" << t.price << " qty=" << t.quantity << "\n";
        }
    };

    std::ostringstream out;
    for (const auto& c : cmds) {
        buf.clear();
        switch (c.kind) {
            case Kind::New:
            case Kind::Market: {
                NewOrder o{
                    .id = c.id,
                    .price = c.price,
                    .quantity = c.quantity,
                    .participant = c.participant,
                    .side = c.side,
                    .type = c.type,
                };
                const SubmitStatus st = book.submit(o, buf);
                EXPECT_FALSE(buf.overflowed()) << "trade buffer overflow on order " << c.id;
                emitTrades(out);
                if (st != SubmitStatus::Ok) {
                    out << "REJECT " << c.id << " " << static_cast<int>(st) << "\n";
                }
                break;
            }
            case Kind::Cancel: {
                const OrderResult r = book.cancel(c.id);
                if (r.status == SubmitStatus::Ok) {
                    out << "CANCEL " << c.id << " OK\n";
                } else {
                    out << "CANCEL " << c.id << " REJECT " << static_cast<int>(r.status) << "\n";
                }
                break;
            }
            case Kind::Replace: {
                NewOrder fresh{
                    .id = c.newId,
                    .price = c.price,
                    .quantity = c.quantity,
                    .participant = c.participant,
                    .side = c.side,
                };
                const OrderResult r = book.replace(c.id, fresh, buf);
                EXPECT_FALSE(buf.overflowed()) << "trade buffer overflow on replace " << c.id;
                emitTrades(out);
                if (r.status == SubmitStatus::Ok) {
                    out << "REPLACE " << c.id << " " << c.newId << " OK\n";
                } else {
                    out << "REPLACE " << c.id << " " << c.newId << " REJECT "
                        << static_cast<int>(r.status) << "\n";
                }
                break;
            }
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

// --- Spec 005 T3: the same scenarios, driven through SpscRing + MatchingThread ---------------
//
// This proves the ring changed nothing OBSERVABLE: same commands, same loadScenario(), same
// golden files -- only the path from command to OrderBook differs. Any missed release-store or
// mis-cached cursor in ipc::SpscRing would show up here as a diverging trade or a stuck test.
//
// The matching thread runs concurrently with this driver, so the driver cannot simply serialize
// events as they are produced without racing the matching thread's publish order. Instead it
// uses MatchingThread::processedCount() (Spec 005 test instrumentation): after processedCount()
// passes the index of the command just pushed, EVERY outbound event that command will ever
// produce has already been release-stored into the outbound ring, so draining the ring at that
// point yields exactly (and only) that command's events, in order.
std::string runScenarioThroughRing(const std::vector<Command>& cmds) {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;

    ipc::SpscRing<ipc::Command> in;
    // MulticastRing<OutboundEvent, 2> (Spec 005 T4's decision): only consumer index 0 is read
    // for this test's output text, but index 1 (the other future consumer) must also be drained
    // or the producer's min-gate will eventually see it as backpressure.
    runtime::MatchingThread<>::OutRing out;
    runtime::MatchingThread<> mt(in, out, cfg);
    mt.start();

    std::ostringstream text;

    for (std::size_t i = 0; i < cmds.size(); ++i) {
        const Command& c = cmds[i];

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

        while (!in.push(ic)) {
            std::this_thread::yield();
        }
        while (mt.processedCount() <= i) {
            std::this_thread::yield();
        }

        std::vector<Trade> trades;
        SubmitStatus status = SubmitStatus::Ok;
        bool hasStatus = false;

        // Consumer index 0 drives this test's output; index 1 is drained in lock-step purely
        // to keep the producer's min-gate from seeing it as a stalled consumer.
        const ipc::OutboundEvent* ev;
        while ((ev = out.tryPeek(0)) != nullptr) {
            if (ev->kind == ipc::OutboundKind::TradeEvent) {
                trades.push_back(ev->payload.trade);
            } else {
                status = ev->payload.statusChange.status;
                hasStatus = true;
            }
            out.consume(0);
        }
        while (out.tryPeek(1) != nullptr) {
            out.consume(1);
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

    text << "BOOK bestBid=" << mt.book().bestBid() << " bestAsk=" << mt.book().bestAsk()
         << " resting=" << mt.book().restingOrders() << "\n";

    mt.stop();
    return text.str();
}

void runGoldenThroughRing(const std::string& name) {
    const fs::path dir = fs::path(VELOX_REPLAY_DIR);
    const fs::path scenario = dir / "scenarios" / (name + ".txt");
    const fs::path golden = dir / "golden" / (name + ".golden");

    const std::string produced = runScenarioThroughRing(loadScenario(scenario));

    std::ifstream gf(golden);
    ASSERT_TRUE(gf.good()) << "missing golden file: " << golden;
    std::stringstream expected;
    expected << gf.rdbuf();

    EXPECT_EQ(produced, expected.str())
        << "Through-the-ring replay diverged from the direct-call golden for scenario: " << name;
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

// --- Spec 002 scenarios -----------------------------------------------------------------------
TEST(GoldenReplay, Cancel) {
    runGolden("cancel");
}
TEST(GoldenReplay, CancelReplace) {
    runGolden("cancel_replace");
}
TEST(GoldenReplay, MarketOrder) {
    runGolden("market_order");
}
TEST(GoldenReplay, Ioc) {
    runGolden("ioc");
}
TEST(GoldenReplay, Fok) {
    runGolden("fok");
}
TEST(GoldenReplay, SelfTradePrevention) {
    runGolden("self_trade_prevention");
}
TEST(GoldenReplay, MarketIntoEmptyBook) {
    runGolden("market_into_empty_book");
}
TEST(GoldenReplay, FokAlmostFills) {
    runGolden("fok_almost_fills");
}
TEST(GoldenReplay, CancelAfterFill) {
    runGolden("cancel_after_fill");
}
TEST(GoldenReplay, ReplaceIntoCross) {
    runGolden("replace_into_cross");
}
TEST(GoldenReplay, StpMultiLevel) {
    runGolden("stp_multi_level");
}
TEST(GoldenReplay, MarketExhaustsBook) {
    runGolden("market_exhausts_book");
}
TEST(GoldenReplay, CancelReplaceResetsPriority) {
    runGolden("cancel_replace_resets_priority");
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

// --- Spec 005 T3: every scenario above, again, driven through the ring -----------------------
TEST(GoldenReplayThroughRing, LimitMatch) {
    runGoldenThroughRing("limit_match");
}
TEST(GoldenReplayThroughRing, PartialFill) {
    runGoldenThroughRing("partial_fill");
}
TEST(GoldenReplayThroughRing, FullFill) {
    runGoldenThroughRing("full_fill");
}
TEST(GoldenReplayThroughRing, CrossingBook) {
    runGoldenThroughRing("crossing_book");
}
TEST(GoldenReplayThroughRing, EmptyBook) {
    runGoldenThroughRing("empty_book");
}
TEST(GoldenReplayThroughRing, FifoSamePrice) {
    runGoldenThroughRing("fifo_same_price");
}
TEST(GoldenReplayThroughRing, PriceImprovement) {
    runGoldenThroughRing("price_improvement");
}
TEST(GoldenReplayThroughRing, Cancel) {
    runGoldenThroughRing("cancel");
}
TEST(GoldenReplayThroughRing, CancelReplace) {
    runGoldenThroughRing("cancel_replace");
}
TEST(GoldenReplayThroughRing, MarketOrder) {
    runGoldenThroughRing("market_order");
}
TEST(GoldenReplayThroughRing, Ioc) {
    runGoldenThroughRing("ioc");
}
TEST(GoldenReplayThroughRing, Fok) {
    runGoldenThroughRing("fok");
}
TEST(GoldenReplayThroughRing, SelfTradePrevention) {
    runGoldenThroughRing("self_trade_prevention");
}
TEST(GoldenReplayThroughRing, MarketIntoEmptyBook) {
    runGoldenThroughRing("market_into_empty_book");
}
TEST(GoldenReplayThroughRing, FokAlmostFills) {
    runGoldenThroughRing("fok_almost_fills");
}
TEST(GoldenReplayThroughRing, CancelAfterFill) {
    runGoldenThroughRing("cancel_after_fill");
}
TEST(GoldenReplayThroughRing, ReplaceIntoCross) {
    runGoldenThroughRing("replace_into_cross");
}
TEST(GoldenReplayThroughRing, StpMultiLevel) {
    runGoldenThroughRing("stp_multi_level");
}
TEST(GoldenReplayThroughRing, MarketExhaustsBook) {
    runGoldenThroughRing("market_exhausts_book");
}
TEST(GoldenReplayThroughRing, CancelReplaceResetsPriority) {
    runGoldenThroughRing("cancel_replace_resets_priority");
}
