#pragma once

// Spec 003 invariant checker.
//
// This is test-only code (tests/invariant/), never linked into the engine, so none of the
// zero-allocation / no-exceptions / no-logging rules that govern engine/ and book/ apply here.
// It is deliberately allowed to be slow (I7 is an O(slots) scan) and to use std::unordered_map,
// std::string, std::ostringstream -- clarity of a failure report matters far more than the
// speed of the checker that produces it.
//
// checkAll() is the single entry point, called by the harness after EVERY operation (NFR-21).
// A non-nullopt Violation is a hard, fatal failure -- the caller FAILs the test and hands the
// report to the shrinker.

#include <cstddef>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "engine/order_book.hpp"

namespace velox::invariant {

// Rejects that happen BEFORE `++seq_` in submitEx() -- see order_book.cpp. Every other status
// (including RejectedPoolExhausted, which happens after matching) means the arrival consumed a
// sequence number.
inline bool isPreSeqReject(SubmitStatus s) noexcept {
    switch (s) {
        case SubmitStatus::RejectedInvalidQuantity:
        case SubmitStatus::RejectedPriceOutOfRange:
        case SubmitStatus::RejectedDuplicateId:
        case SubmitStatus::RejectedMarketIntoEmptyBook:
        case SubmitStatus::RejectedFokUnfillable:
            return true;
        default:
            return false;
    }
}

struct Violation {
    int id = 0;
    const char* name = "";
    std::size_t opIndex = 0;
    std::string detail;
};

struct LedgerEntry {
    Quantity submitted = 0;
    Quantity filledAggressor = 0;
    Quantity filledPassive = 0;
    bool accepted = false;  // false <=> this id's last submit was a pre-seq reject
};

// The harness-side model, updated from each op's OrderResult and the trades it emitted. See
// .claude/plans/003-invariants-property-tests.md for the accounting rules this encodes.
class Ledger {
 public:
    explicit Ledger(StpPolicy stp) noexcept : stp_(stp) {}

    // Call for every NEW/MARKET submit AND for the "fresh" leg of a REPLACE (i.e. whenever
    // submitEx() was actually invoked). `wasAlreadyResting` must be
    // `book.orders().find(id) != nullptr` read BEFORE the call -- a pre-seq reject (including
    // RejectedDuplicateId, but also e.g. RejectedInvalidQuantity, which order_book.cpp checks
    // BEFORE the duplicate-id check) leaves an already-resting order under this id completely
    // untouched, so the ledger entry must not be touched either. Otherwise this is a genuinely
    // new logical order under `id` (fresh, or a reused numeric id freed by a prior
    // cancel/fill), so fill counters reset.
    void onSubmit(OrderId id, Quantity qty, const OrderResult& r, bool wasAlreadyResting) {
        const bool preSeqReject = isPreSeqReject(r.status);
        if (preSeqReject && wasAlreadyResting) {
            expectedSeqDelta_ = 0;
            return;  // the existing resting order under `id` was never touched
        }
        LedgerEntry& e = entries_[id];
        e.submitted = preSeqReject ? 0 : qty;
        e.filledAggressor = 0;
        e.filledPassive = 0;
        e.accepted = !preSeqReject;
        if (!preSeqReject) {
            totalSubmittedAccepted_ += qty;
        }
        accountResidual(r.status, r.remaining);
        expectedSeqDelta_ = preSeqReject ? 0 : 1;
    }

    // Call for a plain CANCEL, with cancel()'s own OrderResult.
    void onCancel(const OrderResult& r) {
        if (r.status == SubmitStatus::Ok) {
            totalWithdrawn_ += r.remaining;
        }
        expectedSeqDelta_ = 0;  // cancel() never touches seq_
    }

    // Call for a REPLACE whose own pre-checks rejected it before cancel()/submitEx() ran (the
    // book is provably untouched in that case -- order_book.cpp's "validate everything first").
    void onReplaceRejectedBeforeDispatch() { expectedSeqDelta_ = 0; }

    // Call for a REPLACE that reached cancel()+submitEx(), with the OLD order's `remaining`
    // read from `book.orders().find(oldId)->remaining` BEFORE replace() was invoked --
    // replace() discards cancel()'s own result internally (order_book.cpp:309), so this is the
    // only place that quantity is observable. Only meaningful under CancelAggressor: only there
    // is every quantity departure guaranteed observable through a returned OrderResult.
    void onReplaceOldWithdrawn(Quantity oldRemainingBeforeReplace) {
        totalWithdrawn_ += oldRemainingBeforeReplace;
    }

    // Call once per op with whatever trades it emitted (possibly empty).
    void onTrades(const TradeBuffer& trades) {
        for (std::size_t i = 0; i < trades.count; ++i) {
            const Trade& t = trades.data[i];
            entries_[t.aggressorId].filledAggressor += t.quantity;
            entries_[t.passiveId].filledPassive += t.quantity;
            totalTradedQty_ += t.quantity;
        }
    }

    const LedgerEntry* find(OrderId id) const noexcept {
        auto it = entries_.find(id);
        return it == entries_.end() ? nullptr : &it->second;
    }

    template<class F>
    void forEachEntry(F&& f) const {
        for (const auto& [id, e] : entries_) {
            f(id, e);
        }
    }

    StpPolicy stp() const noexcept { return stp_; }
    Quantity totalSubmittedAccepted() const noexcept { return totalSubmittedAccepted_; }
    Quantity totalTradedQty() const noexcept { return totalTradedQty_; }
    Quantity totalWithdrawn() const noexcept { return totalWithdrawn_; }
    Seq prevSeq() const noexcept { return prevSeq_; }
    Seq expectedSeqDelta() const noexcept { return expectedSeqDelta_; }
    void commitSeq(Seq newSeq) noexcept { prevSeq_ = newSeq; }

 private:
    void accountResidual(SubmitStatus status, Quantity remaining) {
        switch (status) {
            case SubmitStatus::CancelledResidual:
            case SubmitStatus::CancelledBySelfTradePrevention:
            case SubmitStatus::RejectedPoolExhausted:
                totalWithdrawn_ += remaining;
                break;
            default:
                break;
        }
    }

    StpPolicy stp_;
    std::unordered_map<OrderId, LedgerEntry> entries_;
    Quantity totalSubmittedAccepted_ = 0;
    Quantity totalTradedQty_ = 0;
    Quantity totalWithdrawn_ = 0;
    Seq prevSeq_ = 0;
    Seq expectedSeqDelta_ = 0;
};

namespace detail {

inline std::optional<Violation> fail(int id, const char* name, std::size_t opIndex,
                                     std::string detail) {
    return Violation{id, name, opIndex, std::move(detail)};
}

// I1: quantity conservation.
inline std::optional<Violation> checkQuantityConservation(const OrderBook& book,
                                                          const Ledger& ledger,
                                                          std::size_t opIndex) {
    std::optional<Violation> violation;

    book.orders().forEach([&](OrderId id, Order* o) {
        if (violation) return;
        const LedgerEntry* e = ledger.find(id);
        if (e == nullptr) {
            violation = fail(1, "I1.QuantityConservation", opIndex,
                             "resting order " + std::to_string(id) + " has no ledger entry");
            return;
        }
        const Quantity expected = e->submitted - e->filledAggressor - e->filledPassive;
        if (o->remaining != expected) {
            std::ostringstream d;
            d << "order " << id << ": remaining=" << o->remaining
              << " but submitted-filled=" << expected << " (submitted=" << e->submitted
              << " filledAgg=" << e->filledAggressor << " filledPass=" << e->filledPassive << ")";
            violation = fail(1, "I1.QuantityConservation", opIndex, d.str());
        }
    });
    if (violation) return violation;

    ledger.forEachEntry([&](OrderId id, const LedgerEntry& e) {
        if (violation) return;
        const Quantity filled = e.filledAggressor + e.filledPassive;
        if (filled > e.submitted) {
            std::ostringstream d;
            d << "order " << id << ": filled=" << filled << " > submitted=" << e.submitted;
            violation = fail(1, "I1.QuantityConservation", opIndex, d.str());
        }
    });
    if (violation) return violation;

    // Global ledger cross-check: only valid under CancelAggressor, where every quantity
    // departure is observable through a returned OrderResult (see Ledger's doc comments).
    if (ledger.stp() == StpPolicy::CancelAggressor) {
        Quantity restingSum = 0;
        book.orders().forEach([&](OrderId, Order* o) { restingSum += o->remaining; });
        const Quantity lhs = restingSum + ledger.totalTradedQty() * 2 + ledger.totalWithdrawn();
        const Quantity rhs = ledger.totalSubmittedAccepted();
        if (lhs != rhs) {
            std::ostringstream d;
            d << "global: resting=" << restingSum << " + 2*traded=" << (ledger.totalTradedQty() * 2)
              << " + withdrawn=" << ledger.totalWithdrawn() << " = " << lhs
              << " != submitted(accepted)=" << rhs;
            return fail(1, "I1.QuantityConservationGlobal", opIndex, d.str());
        }
    }
    return std::nullopt;
}

// I2: sequence monotonicity + uniqueness/bound of resting seqs.
inline std::optional<Violation> checkSequenceMonotonicity(const OrderBook& book, Ledger& ledger,
                                                          std::size_t opIndex) {
    const Seq expected = ledger.prevSeq() + ledger.expectedSeqDelta();
    if (book.lastSeq() != expected) {
        std::ostringstream d;
        d << "lastSeq=" << book.lastSeq() << " expected " << ledger.prevSeq() << " + "
          << ledger.expectedSeqDelta() << " = " << expected;
        return fail(2, "I2.SequenceMonotonicity", opIndex, d.str());
    }

    std::unordered_set<Seq> seen;
    std::optional<Violation> violation;
    book.orders().forEach([&](OrderId id, Order* o) {
        if (violation) return;
        if (o->seq > book.lastSeq()) {
            violation = fail(2, "I2.SequenceMonotonicity", opIndex,
                             "order " + std::to_string(id) + " has seq " + std::to_string(o->seq) +
                                 " > lastSeq " + std::to_string(book.lastSeq()));
            return;
        }
        if (!seen.insert(o->seq).second) {
            violation = fail(
                2, "I2.SequenceMonotonicity", opIndex,
                "duplicate seq " + std::to_string(o->seq) + " (order " + std::to_string(id) + ")");
        }
    });
    if (violation) return violation;

    ledger.commitSeq(book.lastSeq());
    return std::nullopt;
}

// I3: no crossed book at the operation boundary.
inline std::optional<Violation> checkNoCrossedBook(const OrderBook& book, std::size_t opIndex) {
    if (!(book.bestBid() < book.bestAsk())) {
        std::ostringstream d;
        d << "bestBid=" << book.bestBid() << " bestAsk=" << book.bestAsk();
        return fail(3, "I3.NoCrossedBook", opIndex, d.str());
    }
    return std::nullopt;
}

// Walk one level head->tail, checking strict seq ordering, back-link integrity, and that the
// level's own aggregates (count, totalQuantity) match the chain. Shared by I4/I6/I8.
inline std::optional<Violation> walkLevel(const PriceLevel& level, Side side, std::size_t opIndex) {
    if (level.empty()) {
        if (level.head() != nullptr || level.tail() != nullptr || level.count() != 0 ||
            level.totalQuantity() != 0) {
            std::ostringstream d;
            d << "price " << level.price()
              << " reports empty() but head/tail/count/qty=" << (level.head() != nullptr) << "/"
              << (level.tail() != nullptr) << "/" << level.count() << "/" << level.totalQuantity();
            return fail(6, "I6.NoEmptyLevelOccupied", opIndex, d.str());
        }
        return std::nullopt;
    }

    Order* prev = nullptr;
    std::size_t chainLen = 0;
    Quantity chainQty = 0;
    Seq lastSeq = -1;
    for (Order* o = level.head(); o != nullptr; o = o->next) {
        if (o->prev != prev) {
            std::ostringstream d;
            d << "price " << level.price() << " order " << o->id << ": back-link mismatch";
            return fail(4, "I4.FifoFairness", opIndex, d.str());
        }
        if (o->side != side) {
            std::ostringstream d;
            d << "price " << level.price() << " order " << o->id << ": resting on wrong side";
            return fail(5, "I5.IdMapLevelConsistency", opIndex, d.str());
        }
        if (chainLen > 0 && o->seq <= lastSeq) {
            std::ostringstream d;
            d << "price " << level.price() << " order " << o->id << ": seq " << o->seq
              << " not strictly greater than predecessor's " << lastSeq;
            return fail(4, "I4.FifoFairness", opIndex, d.str());
        }
        lastSeq = o->seq;
        chainQty += o->remaining;
        prev = o;
        ++chainLen;
        if (chainLen > level.count() + 1) {
            return fail(8, "I8.LevelAggregateAndPool", opIndex,
                        "price " + std::to_string(level.price()) + ": chain longer than count()");
        }
    }
    if (level.tail() != prev) {
        std::ostringstream d;
        d << "price " << level.price() << ": tail() does not match end of chain";
        return fail(4, "I4.FifoFairness", opIndex, d.str());
    }
    if (chainLen != level.count()) {
        std::ostringstream d;
        d << "price " << level.price() << ": chain length " << chainLen << " != count() "
          << level.count();
        return fail(8, "I8.LevelAggregateAndPool", opIndex, d.str());
    }
    if (chainQty != level.totalQuantity()) {
        std::ostringstream d;
        d << "price " << level.price() << ": sum(remaining)=" << chainQty
          << " != totalQuantity()=" << level.totalQuantity();
        return fail(8, "I8.LevelAggregateAndPool", opIndex, d.str());
    }
    return std::nullopt;
}

// I4 (structural part) / I6 / I8, walked over both sides.
inline std::optional<Violation> checkLevelStructure(const OrderBook& book, std::size_t opIndex) {
    for (const Side side : {Side::Buy, Side::Sell}) {
        const book::LevelMap& lm = book.sideView(side);
        for (std::size_t i = 0; i < lm.slots(); ++i) {
            if (auto v = walkLevel(*lm.levelAtIndex(i), side, opIndex)) {
                return v;
            }
        }
    }
    return std::nullopt;
}

// I4 (queue-jump part): after this op, no order remaining at a trade's price has a smaller seq
// than the passive order that just traded there -- unless that passive order was itself removed
// this op (fully filled, or an STP victim).
inline std::optional<Violation> checkNoQueueJump(const OrderBook& book, const TradeBuffer* trades,
                                                 std::size_t opIndex) {
    if (trades == nullptr) return std::nullopt;
    for (std::size_t i = 0; i < trades->count; ++i) {
        const Trade& t = trades->data[i];
        const Order* passive = book.orders().find(t.passiveId);
        if (passive == nullptr) {
            continue;  // removed this op (fully filled, or STP victim) -- nothing to check
        }
        const Side passiveSide = velox::opposite(t.aggressorSide);
        const PriceLevel* level = book.sideView(passiveSide).levelAt(t.price);
        if (level == nullptr) continue;
        for (const Order* o = level->head(); o != nullptr; o = o->next) {
            if (o->seq < passive->seq) {
                std::ostringstream d;
                d << "price " << t.price << ": order " << o->id << " (seq " << o->seq
                  << ") still resting ahead of trade " << t.id << "'s passive order " << t.passiveId
                  << " (seq " << passive->seq << ")";
                return fail(4, "I4.FifoFairness.NoQueueJump", opIndex, d.str());
            }
        }
    }
    return std::nullopt;
}

// I5: OrderIdMap <-> level mutual consistency, plus the count cross-check.
inline std::optional<Violation> checkIdMapLevelConsistency(const OrderBook& book,
                                                           std::size_t opIndex) {
    std::optional<Violation> violation;

    // Forward: every id-map entry is reachable from its level, at the right price/side/slot.
    book.orders().forEach([&](OrderId id, Order* o) {
        if (violation) return;
        if (o->level == nullptr) {
            violation = fail(5, "I5.IdMapLevelConsistency", opIndex,
                             "order " + std::to_string(id) + " has null level pointer");
            return;
        }
        if (o->level->price() != o->price) {
            violation = fail(5, "I5.IdMapLevelConsistency", opIndex,
                             "order " + std::to_string(id) + ": level price " +
                                 std::to_string(o->level->price()) + " != order price " +
                                 std::to_string(o->price));
            return;
        }
        const PriceLevel* atSide = book.sideView(o->side).levelAt(o->price);
        if (atSide != o->level) {
            violation = fail(5, "I5.IdMapLevelConsistency", opIndex,
                             "order " + std::to_string(id) +
                                 ": level pointer does not match sideView(side).levelAt(price)");
            return;
        }
        bool reachable = false;
        for (const Order* w = o->level->head(); w != nullptr; w = w->next) {
            if (w == o) {
                reachable = true;
                break;
            }
        }
        if (!reachable) {
            violation = fail(5, "I5.IdMapLevelConsistency", opIndex,
                             "order " + std::to_string(id) + " not reachable from its own level");
        }
    });
    if (violation) return violation;

    // Reverse: every order reachable in any level is in the id map, under its own id, same pointer.
    std::size_t levelOrderCount = 0;
    for (const Side side : {Side::Buy, Side::Sell}) {
        const book::LevelMap& lm = book.sideView(side);
        for (std::size_t i = 0; i < lm.slots(); ++i) {
            const PriceLevel* level = lm.levelAtIndex(i);
            for (const Order* o = level->head(); o != nullptr; o = o->next) {
                ++levelOrderCount;
                const Order* mapped = book.orders().find(o->id);
                if (mapped != o) {
                    std::ostringstream d;
                    d << "order " << o->id << " in level chain at " << level->price()
                      << " but idMap.find() returns a different (or null) pointer";
                    return fail(5, "I5.IdMapLevelConsistency", opIndex, d.str());
                }
            }
        }
    }
    if (levelOrderCount != book.restingOrders()) {
        std::ostringstream d;
        d << "sum(level counts)=" << levelOrderCount
          << " != restingOrders()=" << book.restingOrders();
        return fail(5, "I5.IdMapLevelConsistency", opIndex, d.str());
    }
    if (book.orders().size() != book.restingOrders()) {
        std::ostringstream d;
        d << "idMap.size()=" << book.orders().size()
          << " != restingOrders()=" << book.restingOrders();
        return fail(5, "I5.IdMapLevelConsistency", opIndex, d.str());
    }
    return std::nullopt;
}

// I7: best price equals the actual best occupied level (full O(slots) scan, deliberately).
inline std::optional<Violation> checkBestPriceIsReal(const OrderBook& book, std::size_t opIndex) {
    for (const Side side : {Side::Buy, Side::Sell}) {
        const book::LevelMap& lm = book.sideView(side);
        Price actualBest = emptySentinel(side);
        if (side == Side::Buy) {
            for (std::size_t i = lm.slots(); i-- > 0;) {
                if (!lm.levelAtIndex(i)->empty()) {
                    actualBest = lm.levelAtIndex(i)->price();
                    break;
                }
            }
        } else {
            for (std::size_t i = 0; i < lm.slots(); ++i) {
                if (!lm.levelAtIndex(i)->empty()) {
                    actualBest = lm.levelAtIndex(i)->price();
                    break;
                }
            }
        }
        if (lm.best() != actualBest) {
            std::ostringstream d;
            d << (side == Side::Buy ? "bid" : "ask") << ": tracked best=" << lm.best()
              << " but actual best occupied level=" << actualBest;
            return fail(7, "I7.BestPriceIsReal", opIndex, d.str());
        }
    }
    return std::nullopt;
}

// I8 (pool half): pool.inUse() matches the resting-order count. The level-aggregate half is
// checked inline by walkLevel() as part of checkLevelStructure().
inline std::optional<Violation> checkPoolAccounting(const OrderBook& book, std::size_t opIndex) {
    if (book.pool().inUse() != book.orders().size()) {
        std::ostringstream d;
        d << "pool.inUse()=" << book.pool().inUse() << " != idMap.size()=" << book.orders().size();
        return fail(8, "I8.LevelAggregateAndPool", opIndex, d.str());
    }
    return std::nullopt;
}

// I9 (Spec 004): LevelMap's occupancy bitset must agree with PriceLevel::empty() for EVERY
// slot. This is the property that catches a missed bit-clear/bit-set in the hierarchical
// bitset that replaced the linear nextOccupied() scan -- exactly the class of bug that would
// otherwise surface as a wrong best price weeks later, on some book shape this suite's fixed
// scenarios never happened to hit.
inline std::optional<Violation> checkOccupancyBitsetConsistency(const OrderBook& book,
                                                                std::size_t opIndex) {
    for (const Side side : {Side::Buy, Side::Sell}) {
        const book::LevelMap& lm = book.sideView(side);
        for (std::size_t i = 0; i < lm.slots(); ++i) {
            const bool bit = lm.occupiedBit(i);
            const bool nonEmpty = !lm.levelAtIndex(i)->empty();
            if (bit != nonEmpty) {
                std::ostringstream d;
                d << (side == Side::Buy ? "bid" : "ask") << " slot " << i
                  << ": occupancy bit=" << bit << " but level.empty()=" << !nonEmpty;
                return fail(9, "I9.OccupancyBitsetConsistency", opIndex, d.str());
            }
        }
    }
    return std::nullopt;
}

}  // namespace detail

// The single entry point. `trades` is the TradeBuffer emitted by the op just performed (or
// nullptr for ops that cannot emit trades, e.g. a plain cancel) -- needed for I4's queue-jump
// check. Call after EVERY operation (NFR-21).
inline std::optional<Violation> checkAll(const OrderBook& book, Ledger& ledger, std::size_t opIndex,
                                         const TradeBuffer* trades = nullptr) {
    if (auto v = detail::checkQuantityConservation(book, ledger, opIndex)) return v;
    if (auto v = detail::checkSequenceMonotonicity(book, ledger, opIndex)) return v;
    if (auto v = detail::checkNoCrossedBook(book, opIndex)) return v;
    if (auto v = detail::checkLevelStructure(book, opIndex)) return v;
    if (auto v = detail::checkNoQueueJump(book, trades, opIndex)) return v;
    if (auto v = detail::checkIdMapLevelConsistency(book, opIndex)) return v;
    if (auto v = detail::checkBestPriceIsReal(book, opIndex)) return v;
    if (auto v = detail::checkPoolAccounting(book, opIndex)) return v;
    if (auto v = detail::checkOccupancyBitsetConsistency(book, opIndex)) return v;
    return std::nullopt;
}

}  // namespace velox::invariant
