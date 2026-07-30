#pragma once

// Spec 009 T1: the shared steady-state workload, lifted out of velox_ring_bench.cpp
// (steadyStateCommand) and velox_bench.cpp (populate/makeConfig/BM_SweepThinBook's setup) so
// every path driver -- and the two existing saturation benchmarks -- share one definition
// instead of drifting copies. Net pool usage per steady-state command is zero (see
// velox_bench.cpp's BM_SubmitRestingOrder comment for why that discipline exists at all): the
// book never grows, so the pool is never exhausted no matter how long a run lasts.

#include <cstdint>
#include <vector>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"

namespace velox::loadgen {

constexpr Price kMid = 100 * kPriceScale;
constexpr Price kTick = kPriceScale / 100;
constexpr Price kThinGap = 5 * kPriceScale;  // 500 ticks between sparse levels

enum class Workload { Dense, Thin };

inline BookConfig makeConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kTick;
    cfg.maxOrders = 1u << 20;
    return cfg;
}

// Distinct participants (2/3) from the population commands' participant 1, so self-trade
// prevention (the Spec 002 default policy) never fires and the steady-state assumption holds.
inline ipc::Command steadyStateCommand(OrderId id, bool rest) {
    ipc::Command c{};
    c.id = id;
    c.newId = 0;
    c.price = kMid - kTick;
    c.quantity = 10;
    c.participant = rest ? 2 : 3;
    c.kind = ipc::CommandKind::New;
    c.side = rest ? Side::Buy : Side::Sell;
    c.type = OrderType::Limit;
    return c;
}

// Builds real depth on both sides of an in-process book directly (velox_bench.cpp's original
// populate() -- kept in this exact book-mutating form for straight-line microbenchmarks that
// call OrderBook::submit() themselves rather than going through a Command ring).
inline void populate(OrderBook& book, OrderId& id, TradeBuffer& buf, int levelsPerSide) {
    for (int i = 1; i <= levelsPerSide; ++i) {
        buf.clear();
        NewOrder bid{
            .id = id++,
            .price = kMid - kTick * i,
            .quantity = 100,
            .participant = 1,
            .side = Side::Buy,
        };
        book.submit(bid, buf);

        buf.clear();
        NewOrder ask{
            .id = id++,
            .price = kMid + kTick * i,
            .quantity = 100,
            .participant = 1,
            .side = Side::Sell,
        };
        book.submit(ask, buf);
    }
}

// The commands that build real depth before the measured phase starts, for the selected
// workload. Dense: 50 levels each side, adjacent ticks (velox_bench.cpp's populate()). Thin: a
// single-sided ladder $5 apart (velox_bench.cpp's BM_SweepThinBook setup) -- the gap
// LevelMap::nextOccupied() must walk across instead of finding depth one tick away.
inline std::vector<ipc::Command> populationCommands(Workload w, OrderId& nextId) {
    std::vector<ipc::Command> cmds;
    if (w == Workload::Dense) {
        cmds.reserve(100);
        for (int i = 1; i <= 50; ++i) {
            cmds.push_back(ipc::Command{.id = nextId++,
                                        .newId = 0,
                                        .price = kMid - kTick * i,
                                        .quantity = 100,
                                        .participant = 1,
                                        .kind = ipc::CommandKind::New,
                                        .side = Side::Buy,
                                        .type = OrderType::Limit});
            cmds.push_back(ipc::Command{.id = nextId++,
                                        .newId = 0,
                                        .price = kMid + kTick * i,
                                        .quantity = 100,
                                        .participant = 1,
                                        .kind = ipc::CommandKind::New,
                                        .side = Side::Sell,
                                        .type = OrderType::Limit});
        }
    } else {
        for (Price p = kMid; p >= (1 * kPriceScale); p -= kThinGap) {
            cmds.push_back(ipc::Command{.id = nextId++,
                                        .newId = 0,
                                        .price = p,
                                        .quantity = 10,
                                        .participant = 1,
                                        .kind = ipc::CommandKind::New,
                                        .side = Side::Buy,
                                        .type = OrderType::Limit});
        }
    }
    return cmds;
}

// The best (highest) bid price of a Thin population -- the level the steady-state cycle below
// repeatedly consumes and replenishes. populationCommands() inserts kMid first and walks
// downward, so kMid is always the highest price actually inserted.
inline Price thinTopPrice() {
    return kMid;
}

// One steady-state order generator, workload-aware. Dense alternates a resting bid with a
// crossing sell (net-zero pool usage per pair, same discipline as BM_SubmitRestingOrder). Thin
// alternates fully consuming the current best bid (forcing the gap walk) with replenishing the
// exact price just vacated (same discipline as BM_SweepThinBook) -- also net-zero per pair.
class SteadyStateGenerator {
 public:
    SteadyStateGenerator(Workload w, Price thinTop) : workload_(w), topPrice_(thinTop) {}

    ipc::Command next(OrderId id) {
        const bool first = (counter_ % 2) == 0;
        ++counter_;
        if (workload_ == Workload::Dense) {
            return steadyStateCommand(id, first);
        }
        ipc::Command c{};
        c.id = id;
        c.newId = 0;
        c.quantity = 10;
        c.kind = ipc::CommandKind::New;
        c.type = OrderType::Limit;
        if (first) {
            // Fully consume the current best bid -- willing to sell down to the floor.
            c.price = 1 * kPriceScale;
            c.participant = 2;
            c.side = Side::Sell;
        } else {
            // Replenish the vacated top price so the same sparse ladder repeats forever.
            c.price = topPrice_;
            c.participant = 1;
            c.side = Side::Buy;
        }
        return c;
    }

 private:
    Workload workload_;
    Price topPrice_;
    std::uint64_t counter_ = 0;
};

}  // namespace velox::loadgen
