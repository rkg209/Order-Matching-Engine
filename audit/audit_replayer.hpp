#pragma once

// Shadow-replay for the audit tier (Spec 012, T2). Owns its own OrderBook, completely
// independent of the live matching thread's, and turns journal commands into typed rows -- the
// exact same pattern as `sequencer::SnapshotThread::replayUpTo` (Spec 006), pointed at a
// different sink. Determinism (constitution P4) is what makes this sound: the shadow book here
// is, by construction, byte-identical in behavior to the live book that originally produced the
// journal being replayed.
//
// Two things the existing SnapshotThread precedent gets away with that this cannot:
//
// 1. SnapshotThread's `Trade storage[64]` (`snapshot_thread.hpp:94-95`) discards every trade --
//    it only wants final book state, so silent truncation past 64 trades in one sweep is
//    invisible to it. An audit trail that drops trades is not an audit trail, so this replayer
//    sizes its TradeBuffer to the book's own max-resting-orders bound (`BookConfig::maxOrders`),
//    which is a true upper bound on how many resting orders a single aggressive order can cross
//    in one sweep -- the buffer can never overflow.
// 2. There are no timestamps anywhere in `ipc::Command`, `Trade`, or `OutboundEvent` -- globalSeq
//    is deliberately ring-arrival order, never wall-clock, to keep the engine deterministic.
//    `TradeRow` therefore carries no `tradedAt` field; `globalSeq` is the only ordering key, and
//    it is the caller's job (T4/T3) to timestamp ingestion, never the trade itself.
//
// No libpq anywhere in this header -- it links only velox_engine + velox_sequencer, so the
// determinism test (T7.2) runs with no database installed.

#include <cassert>
#include <cstdint>
#include <vector>

#include "engine/order_book.hpp"
#include "engine/trade.hpp"
#include "ipc/command.hpp"
#include "sequencer/journal_tailer.hpp"

namespace velox::audit {

// One row per journal command. `orderId`/`price`/`quantity` describe the order as submitted
// (New), the id being removed (Cancel), or the REPLACEMENT (Replace) -- `ipc::Command` has only
// one price/quantity slot, and for Replace that slot is always the new order's, never the old
// one's (see `ipc::toNewOrder`). `newOrderId` is populated only for Replace.
struct OrderEventRow {
    Seq globalSeq = 0;
    std::uint32_t shardId = 0;
    ipc::CommandKind kind = ipc::CommandKind::New;
    OrderId orderId = 0;
    OrderId newOrderId = 0;  // Replace only; 0 otherwise (0 is never a valid order id).
    ParticipantId participant = 0;
    Side side = Side::Buy;
    OrderType orderType = OrderType::Limit;
    Price price = 0;
    Quantity quantity = 0;
};

struct TradeRow {
    Seq globalSeq = 0;  // the command that produced this trade; the ordering key.
    std::uint32_t shardId = 0;
    Seq tradeId = 0;  // Trade::id -- deterministic monotonic counter, never a UUID or a clock.
    OrderId aggressorOrderId = 0;
    OrderId passiveOrderId = 0;
    Price price = 0;
    Quantity quantity = 0;
    Side aggressorSide = Side::Buy;
};

// Applies one journal record to the shadow book and appends whatever rows it produced to
// `orderEvents`/`trades`. The caller (JournalTailer's driving loop) owns pacing and I/O; this
// class only ever touches the shadow book and the two output vectors.
class AuditReplayer {
 public:
    AuditReplayer(std::uint32_t shardId, BookConfig cfg)
        : shardId_(shardId), book_(cfg), tradeStorage_(cfg.maxOrders) {}

    AuditReplayer(const AuditReplayer&) = delete;
    AuditReplayer& operator=(const AuditReplayer&) = delete;

    void apply(const sequencer::TailResult& r, std::vector<OrderEventRow>& orderEvents,
               std::vector<TradeRow>& trades) {
        TradeBuffer buf{tradeStorage_.data(), tradeStorage_.size(), 0};

        OrderEventRow ev;
        ev.globalSeq = r.globalSeq;
        ev.shardId = shardId_;
        ev.kind = r.kind;
        ev.participant = r.command.participant;
        ev.side = r.command.side;
        ev.orderType = r.command.type;
        ev.price = r.command.price;
        ev.quantity = r.command.quantity;

        switch (r.kind) {
            case ipc::CommandKind::New: {
                ev.orderId = r.command.id;
                const NewOrder o = ipc::toNewOrder(r.command);
                book_.submit(o, buf);
                break;
            }
            case ipc::CommandKind::Cancel: {
                ev.orderId = r.command.id;
                book_.cancel(r.command.id);
                break;
            }
            case ipc::CommandKind::Replace: {
                ev.orderId = r.command.id;
                ev.newOrderId = r.command.newId;
                NewOrder fresh = ipc::toNewOrder(r.command);
                fresh.id = r.command.newId;
                book_.replace(r.command.id, fresh, buf);
                break;
            }
        }
        orderEvents.push_back(ev);

        // overflowed() can never be true given tradeStorage_ is sized to cfg.maxOrders -- a
        // single sweep cannot cross more resting orders than the book can hold. Asserted, not
        // silently trusted: a truncated audit trail is the one failure mode this class exists
        // to prevent.
        assert(!buf.overflowed());
        for (std::size_t i = 0; i < buf.count; ++i) {
            const Trade& t = buf.data[i];
            trades.push_back(TradeRow{
                .globalSeq = r.globalSeq,
                .shardId = shardId_,
                .tradeId = t.id,
                .aggressorOrderId = t.aggressorId,
                .passiveOrderId = t.passiveId,
                .price = t.price,
                .quantity = t.quantity,
                .aggressorSide = t.aggressorSide,
            });
        }
    }

 private:
    std::uint32_t shardId_;
    OrderBook book_;
    std::vector<Trade> tradeStorage_;
};

}  // namespace velox::audit
