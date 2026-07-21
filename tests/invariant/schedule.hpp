#pragma once

// Spec 003 randomized schedule generator + harness runner.
//
// All randomness lives HERE, in a seeded std::mt19937_64. Same seed => same schedule, always
// (the engine itself stays strictly deterministic -- Principle 3). Test-only code: none of the
// hot-path rules apply.

#include <array>
#include <cstdint>
#include <optional>
#include <random>
#include <vector>

#include "engine/order_book.hpp"
#include "tests/invariant/invariants.hpp"

namespace velox::invariant {

enum class OpKind { Submit, Cancel, Replace };

struct Op {
    OpKind kind = OpKind::Submit;
    OrderId id = 0;     // Submit: the new order's id. Cancel: the id to cancel. Replace: OLD id.
    OrderId newId = 0;  // Replace only.
    Side side = Side::Buy;
    Price price = 0;
    Quantity qty = 0;
    ParticipantId participant = 0;
    OrderType type = OrderType::Limit;
};

enum class Profile {
    Uniform,
    HeavyCancel,
    SinglePrice,
    AlternatingCross,
    DrainRefill,
    LevelChurn,
    StpHeavy,
    ReplaceHeavy,
    TinyPool,
    NarrowRange,
};

inline const char* profileName(Profile p) noexcept {
    switch (p) {
        case Profile::Uniform:
            return "Uniform";
        case Profile::HeavyCancel:
            return "HeavyCancel";
        case Profile::SinglePrice:
            return "SinglePrice";
        case Profile::AlternatingCross:
            return "AlternatingCross";
        case Profile::DrainRefill:
            return "DrainRefill";
        case Profile::LevelChurn:
            return "LevelChurn";
        case Profile::StpHeavy:
            return "StpHeavy";
        case Profile::ReplaceHeavy:
            return "ReplaceHeavy";
        case Profile::TinyPool:
            return "TinyPool";
        case Profile::NarrowRange:
            return "NarrowRange";
    }
    return "Unknown";
}

struct Schedule {
    std::uint64_t seed = 0;
    Profile profile = Profile::Uniform;
    BookConfig cfg;
    std::vector<Op> ops;
};

namespace detail {

// Knobs a profile dials, rather than bespoke generation logic per profile -- keeps the
// generator's control flow in one place and each profile a data point, not a code path.
struct ProfileParams {
    double cancelProb = 0.2;
    double replaceProb = 0.05;
    double marketProb = 0.05;
    double iocProb = 0.05;
    double fokProb = 0.05;
    double outOfRangeProb = 0.0;
    int idPoolSize = 64;
    int participantCount = 8;
    int priceTicks = 100;  // distinct price points the generator plays with
    Quantity minQty = 1;
    Quantity maxQty = 20;
    std::size_t maxOrders = 1u << 16;
    StpPolicy stp = StpPolicy::CancelAggressor;
    bool alternatingCross = false;
    bool drainRefillBursts = false;
};

inline ProfileParams paramsFor(Profile p) {
    ProfileParams pp;
    switch (p) {
        case Profile::Uniform:
            break;
        case Profile::HeavyCancel:
            pp.cancelProb = 0.8;
            pp.idPoolSize = 32;
            break;
        case Profile::SinglePrice:
            pp.priceTicks = 1;
            pp.idPoolSize = 96;
            break;
        case Profile::AlternatingCross:
            pp.alternatingCross = true;
            pp.priceTicks = 20;
            break;
        case Profile::DrainRefill:
            pp.drainRefillBursts = true;
            pp.idPoolSize = 48;
            break;
        case Profile::LevelChurn:
            pp.priceTicks = 3;
            pp.marketProb = 0.15;
            pp.iocProb = 0.15;
            pp.idPoolSize = 64;
            break;
        case Profile::StpHeavy:
            pp.participantCount = 3;
            pp.priceTicks = 10;
            pp.idPoolSize = 48;
            break;
        case Profile::ReplaceHeavy:
            pp.replaceProb = 0.5;
            pp.cancelProb = 0.1;
            pp.idPoolSize = 40;
            break;
        case Profile::TinyPool:
            pp.maxOrders = 16;
            pp.cancelProb = 0.05;
            pp.idPoolSize = 128;
            break;
        case Profile::NarrowRange:
            pp.priceTicks = 8;
            pp.outOfRangeProb = 0.1;
            break;
    }
    return pp;
}

}  // namespace detail

// Generates a schedule from a seed. `stpOverride`, when set, wins over the profile's default
// StpPolicy -- used to run StpHeavy once per policy (the DoD calls for "one profile instance per
// StpPolicy").
inline Schedule generate(Profile profile, std::uint64_t seed, std::size_t maxOps,
                         std::optional<StpPolicy> stpOverride = std::nullopt) {
    const detail::ProfileParams pp = detail::paramsFor(profile);

    Schedule sched;
    sched.seed = seed;
    sched.profile = profile;
    sched.cfg.minPrice = 1 * kPriceScale;
    sched.cfg.maxPrice = sched.cfg.minPrice + (pp.priceTicks - 1) * (kPriceScale / 100);
    sched.cfg.tick = kPriceScale / 100;
    sched.cfg.maxOrders = pp.maxOrders;
    sched.cfg.stp = stpOverride.value_or(pp.stp);

    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<int> idDist(1, pp.idPoolSize);
    std::uniform_int_distribution<int> tickDist(0, pp.priceTicks - 1);
    std::uniform_int_distribution<Quantity> qtyDist(pp.minQty, pp.maxQty);
    std::uniform_int_distribution<int> partDist(0, pp.participantCount - 1);

    auto randomPrice = [&](bool allowOutOfRange) -> Price {
        if (allowOutOfRange && unit(rng) < pp.outOfRangeProb) {
            return unit(rng) < 0.5 ? sched.cfg.minPrice - sched.cfg.tick
                                   : sched.cfg.maxPrice + sched.cfg.tick;
        }
        return sched.cfg.minPrice + tickDist(rng) * sched.cfg.tick;
    };

    auto randomType = [&]() -> OrderType {
        const double r = unit(rng);
        if (r < pp.fokProb) return OrderType::Fok;
        if (r < pp.fokProb + pp.iocProb) return OrderType::Ioc;
        if (r < pp.fokProb + pp.iocProb + pp.marketProb) return OrderType::Market;
        return OrderType::Limit;
    };

    sched.ops.reserve(maxOps);
    bool drainPhase = false;
    for (std::size_t i = 0; i < maxOps; ++i) {
        if (pp.drainRefillBursts && i % 40 == 0) {
            drainPhase = !drainPhase;
        }
        const double cancelProb = (pp.drainRefillBursts && drainPhase) ? 0.9 : pp.cancelProb;

        Op op;
        const double r = unit(rng);
        if (r < cancelProb) {
            op.kind = OpKind::Cancel;
            op.id = idDist(rng);
        } else if (r < cancelProb + pp.replaceProb) {
            op.kind = OpKind::Replace;
            op.id = idDist(rng);
            op.newId = idDist(rng);
            op.side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
            op.price = randomPrice(pp.outOfRangeProb > 0.0);
            op.qty = qtyDist(rng);
            op.participant = partDist(rng);
        } else {
            op.kind = OpKind::Submit;
            op.id = idDist(rng);
            op.side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
            op.type = randomType();
            op.qty = qtyDist(rng);
            op.participant = partDist(rng);
            if (pp.alternatingCross) {
                const bool aggressive = (i % 2) == 0;
                op.price = aggressive
                               ? (op.side == Side::Buy ? sched.cfg.maxPrice : sched.cfg.minPrice)
                               : (op.side == Side::Buy ? sched.cfg.minPrice : sched.cfg.maxPrice);
            } else {
                op.price = randomPrice(pp.outOfRangeProb > 0.0);
            }
        }
        sched.ops.push_back(op);
    }
    return sched;
}

// FNV-1a over the trade stream. Used to prove same-seed determinism without keeping every
// trade's full text around (SameSeedProducesIdenticalTradeDigest).
inline std::uint64_t digestTrade(std::uint64_t h, const Trade& t) noexcept {
    auto mix = [](std::uint64_t h2, std::uint64_t v) noexcept {
        h2 ^= v;
        h2 *= 0x100000001b3ULL;
        return h2;
    };
    h = mix(h, static_cast<std::uint64_t>(t.id));
    h = mix(h, static_cast<std::uint64_t>(t.aggressorId));
    h = mix(h, static_cast<std::uint64_t>(t.passiveId));
    h = mix(h, static_cast<std::uint64_t>(t.price));
    h = mix(h, static_cast<std::uint64_t>(t.quantity));
    h = mix(h, static_cast<std::uint64_t>(t.aggressorSide));
    return h;
}

struct RunResult {
    std::optional<Violation> violation;
    std::uint64_t tradeDigest = 0xcbf29ce484222325ULL;  // FNV offset basis
};

// Runs `sched` (or, if `limit` is set, only its first `limit` ops -- used by the shrinker to
// probe truncated candidates), checking all invariants after every op, stopping at the first
// violation.
inline RunResult runSchedule(const Schedule& sched,
                             std::optional<std::size_t> limit = std::nullopt) {
    OrderBook book(sched.cfg);
    Ledger ledger(sched.cfg.stp);
    RunResult result;

    std::array<Trade, 256> storage{};
    TradeBuffer buf{storage.data(), storage.size(), 0};

    const std::size_t n = std::min(sched.ops.size(), limit.value_or(sched.ops.size()));
    for (std::size_t i = 0; i < n; ++i) {
        const Op& op = sched.ops[i];
        buf.clear();

        switch (op.kind) {
            case OpKind::Submit: {
                const bool wasAlreadyResting = book.orders().find(op.id) != nullptr;
                NewOrder o{.id = op.id,
                           .price = op.price,
                           .quantity = op.qty,
                           .participant = op.participant,
                           .side = op.side,
                           .type = op.type};
                const OrderResult r = book.submitEx(o, buf);
                if (buf.overflowed()) {
                    buf.count = buf.capacity;  // don't read past storage; the digest below is
                                               // best-effort once truncation itself is a bug
                }
                ledger.onSubmit(op.id, op.qty, r, wasAlreadyResting);
                ledger.onTrades(buf);
                for (std::size_t t = 0; t < buf.count; ++t) {
                    result.tradeDigest = digestTrade(result.tradeDigest, buf.data[t]);
                }
                result.violation = checkAll(book, ledger, i, &buf);
                break;
            }
            case OpKind::Cancel: {
                const OrderResult r = book.cancel(op.id);
                ledger.onCancel(r);
                result.violation = checkAll(book, ledger, i, nullptr);
                break;
            }
            case OpKind::Replace: {
                const Order* old = book.orders().find(op.id);
                const Quantity oldRemaining = (old != nullptr) ? old->remaining : 0;

                NewOrder fresh{.id = op.newId,
                               .price = op.price,
                               .quantity = op.qty,
                               .participant = op.participant,
                               .side = op.side,
                               .type = OrderType::Limit};
                const OrderResult r = book.replace(op.id, fresh, buf);

                const bool rejectedBeforeDispatch =
                    r.status == SubmitStatus::RejectedUnknownOrder ||
                    r.status == SubmitStatus::RejectedInvalidQuantity ||
                    r.status == SubmitStatus::RejectedPriceOutOfRange ||
                    r.status == SubmitStatus::RejectedDuplicateId;

                if (rejectedBeforeDispatch) {
                    ledger.onReplaceRejectedBeforeDispatch();
                } else {
                    ledger.onReplaceOldWithdrawn(oldRemaining);
                    // `newId` is guaranteed not resting at this point: either it differs from
                    // `op.id` and wasn't a duplicate (replace()'s own pre-check would have
                    // caught that as rejectedBeforeDispatch), or it equals `op.id` and was just
                    // freed by cancel() inside replace().
                    ledger.onSubmit(op.newId, op.qty, r, /*wasAlreadyResting=*/false);
                    ledger.onTrades(buf);
                    for (std::size_t t = 0; t < buf.count; ++t) {
                        result.tradeDigest = digestTrade(result.tradeDigest, buf.data[t]);
                    }
                }
                result.violation = checkAll(book, ledger, i, &buf);
                break;
            }
        }

        if (result.violation) {
            return result;
        }
    }
    return result;
}

}  // namespace velox::invariant
