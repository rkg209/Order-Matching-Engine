#pragma once

// Spec 003 shrinker: reduces a failing Schedule to a minimal counterexample, so a violation
// found deep in a 2,000-op random schedule turns into something a human can read and fix the
// same day. Test-only code.

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "tests/invariant/invariants.hpp"
#include "tests/invariant/schedule.hpp"

namespace velox::invariant {

namespace detail {

// The shrink predicate: rerunning the (possibly-edited) schedule still produces a violation
// with the SAME Violation::id -- not the same op index or message, since deleting ops shifts
// indices and can change surface detail while the underlying bug is unchanged. Ops deleted that
// other ops referenced simply become rejects (unknown id), which is harmless.
inline bool reproduces(const Schedule& candidate, int targetViolationId) {
    RunResult r = runSchedule(candidate);
    return r.violation.has_value() && r.violation->id == targetViolationId;
}

}  // namespace detail

struct ShrinkOutcome {
    Schedule schedule;
    Violation violation;
    std::size_t stepsUsed = 0;
};

// `failing` must already reproduce `firstViolation` end-to-end. Shrinks under a step budget
// (default 20,000 re-runs), looping ddmin chunk removal -> single-op sweep -> field
// simplification to a fixpoint.
inline ShrinkOutcome shrink(const Schedule& failing, const Violation& firstViolation,
                            std::size_t stepBudget = 20000) {
    const int targetId = firstViolation.id;
    std::size_t steps = 0;

    // Nothing after the violating op can matter -- it never ran.
    std::vector<Op> ops(
        failing.ops.begin(),
        failing.ops.begin() + std::min(failing.ops.size(), firstViolation.opIndex + 1));

    auto tryOps = [&](const std::vector<Op>& candidateOps) -> bool {
        if (steps >= stepBudget || candidateOps.empty()) return false;
        ++steps;
        Schedule candidate = failing;
        candidate.ops = candidateOps;
        return detail::reproduces(candidate, targetId);
    };

    bool changed = true;
    while (changed && steps < stepBudget) {
        changed = false;

        // Pass 1: ddmin chunk removal -- halves, quarters, ..., down to single ops.
        std::size_t chunkSize = ops.size() / 2;
        while (chunkSize >= 1 && steps < stepBudget) {
            bool removedThisSize = false;
            std::size_t start = 0;
            while (start < ops.size() && steps < stepBudget) {
                const std::size_t end = std::min(start + chunkSize, ops.size());
                std::vector<Op> candidate;
                candidate.reserve(ops.size() - (end - start));
                candidate.insert(candidate.end(), ops.begin(), ops.begin() + start);
                candidate.insert(candidate.end(), ops.begin() + end, ops.end());
                if (tryOps(candidate)) {
                    ops = std::move(candidate);
                    removedThisSize = true;
                    changed = true;
                    // Don't advance `start`: the vector shrank, retry at the same offset.
                } else {
                    start += chunkSize;
                }
            }
            if (!removedThisSize) chunkSize /= 2;
        }

        // Pass 2: single-op sweep (cheap insurance beyond chunkSize==1 above).
        for (std::size_t i = 0; i < ops.size() && steps < stepBudget;) {
            std::vector<Op> candidate = ops;
            candidate.erase(candidate.begin() + static_cast<std::ptrdiff_t>(i));
            if (tryOps(candidate)) {
                ops = std::move(candidate);
                changed = true;
            } else {
                ++i;
            }
        }

        // Pass 3: field simplification, greedily, per surviving op.
        for (std::size_t i = 0; i < ops.size() && steps < stepBudget; ++i) {
            Op& op = ops[i];

            if (op.qty > 1) {
                Quantity lo = 1, hi = op.qty;
                while (lo < hi && steps < stepBudget) {
                    const Quantity mid = lo + (hi - lo) / 2;
                    std::vector<Op> candidate = ops;
                    candidate[i].qty = mid;
                    if (tryOps(candidate)) {
                        hi = mid;
                    } else {
                        lo = mid + 1;
                    }
                }
                if (hi != op.qty) {
                    op.qty = hi;
                    changed = true;
                }
            }

            if (op.kind != OpKind::Cancel && op.price != failing.cfg.minPrice) {
                std::vector<Op> candidate = ops;
                candidate[i].price = failing.cfg.minPrice;
                if (tryOps(candidate)) {
                    op.price = failing.cfg.minPrice;
                    changed = true;
                }
            }

            if (op.participant != 0) {
                std::vector<Op> candidate = ops;
                candidate[i].participant = 0;
                if (tryOps(candidate)) {
                    op.participant = 0;
                    changed = true;
                }
            }

            if (op.type != OrderType::Limit) {
                std::vector<Op> candidate = ops;
                candidate[i].type = OrderType::Limit;
                if (tryOps(candidate)) {
                    op.type = OrderType::Limit;
                    changed = true;
                }
            }

            if (op.kind == OpKind::Replace) {
                Op cancelOp;
                cancelOp.kind = OpKind::Cancel;
                cancelOp.id = op.id;
                Op submitOp;
                submitOp.kind = OpKind::Submit;
                submitOp.id = op.newId;
                submitOp.side = op.side;
                submitOp.price = op.price;
                submitOp.qty = op.qty;
                submitOp.participant = op.participant;
                submitOp.type = OrderType::Limit;

                std::vector<Op> candidate = ops;
                candidate[i] = cancelOp;
                candidate.insert(candidate.begin() + static_cast<std::ptrdiff_t>(i) + 1, submitOp);
                if (tryOps(candidate)) {
                    ops = std::move(candidate);
                    changed = true;
                }
            }
        }
    }

    Schedule shrunk = failing;
    shrunk.ops = ops;
    RunResult final = runSchedule(shrunk);
    return ShrinkOutcome{shrunk, *final.violation, steps};
}

namespace detail {

inline std::string formatPrice(Price p) {
    const bool neg = p < 0;
    const std::int64_t ap = neg ? -p : p;
    const std::int64_t whole = ap / kPriceScale;
    const std::int64_t frac = ap % kPriceScale;
    std::ostringstream ss;
    if (neg) ss << "-";
    ss << whole << "." << std::setfill('0') << std::setw(4) << frac;
    return ss.str();
}

inline const char* sideStr(Side s) noexcept {
    return s == Side::Buy ? "BUY" : "SELL";
}

}  // namespace detail

// Renders a schedule in EXACTLY the replay scenario text format (tests/replay/replay_test.cpp),
// so a shrunk counterexample can be pasted straight into tests/replay/scenarios/ and blessed as
// a permanent regression test.
inline std::string renderAsReplayScenario(const Schedule& sched) {
    std::ostringstream out;
    out << "# shrunk from profile=" << profileName(sched.profile) << " seed=" << sched.seed << "\n";
    for (const Op& op : sched.ops) {
        switch (op.kind) {
            case OpKind::Submit:
                if (op.type == OrderType::Market) {
                    out << "MARKET " << op.id << " " << detail::sideStr(op.side) << " " << op.qty
                        << " " << op.participant << "\n";
                } else {
                    out << "NEW " << op.id << " " << detail::sideStr(op.side) << " "
                        << detail::formatPrice(op.price) << " " << op.qty << " " << op.participant;
                    if (op.type == OrderType::Ioc) out << " IOC";
                    if (op.type == OrderType::Fok) out << " FOK";
                    out << "\n";
                }
                break;
            case OpKind::Cancel:
                out << "CANCEL " << op.id << "\n";
                break;
            case OpKind::Replace:
                out << "REPLACE " << op.id << " " << op.newId << " " << detail::sideStr(op.side)
                    << " " << detail::formatPrice(op.price) << " " << op.qty << " "
                    << op.participant << "\n";
                break;
        }
    }
    return out.str();
}

}  // namespace velox::invariant
