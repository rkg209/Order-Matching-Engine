#pragma once

// The single recovery entry point (Spec 006, FR-20, NFR-24): newest-valid snapshot -> load ->
// journal tail replay from `snapshot.globalSeq + 1` to end -> engine is live. No manual step,
// ever -- there is no repair tool, and there must never be one (a human running a repair tool is
// itself the failure NFR-24 forbids).

#include <cstdint>
#include <filesystem>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "sequencer/journal_reader.hpp"
#include "sequencer/snapshot_reader.hpp"

namespace velox::recovery {

enum class RecoveryStatus { Ok, TornTail, Corrupt };

struct RecoveryResult {
    RecoveryStatus status = RecoveryStatus::Ok;
    Seq lastSeq = 0;  // last durably-applied global sequence number

    // Where a JournalWriter should resume durable appends after this recovery -- only
    // meaningful when hasJournalSegment is true (i.e. the journal directory had at least one
    // segment). A torn tail's resumeOffset is the truncation point; a clean end's is the
    // segment's own size.
    bool hasJournalSegment = false;
    std::filesystem::path resumeSegmentPath;
    std::size_t resumeOffset = 0;
    std::uint64_t resumeSegmentCreatedCounter = 0;
};

class RecoveryManager {
 public:
    RecoveryManager(std::filesystem::path journalDir, std::filesystem::path snapshotDir)
        : journalDir_(std::move(journalDir)), snapshotDir_(std::move(snapshotDir)) {}

    // Rebuilds `book` (already constructed with the live BookConfig) in place. Recovery from an
    // empty journal/snapshot dir is a supported, non-error case: `book` is simply left as
    // constructed (empty, seq 0).
    RecoveryResult recover(OrderBook& book) const {
        RecoveryResult result;
        Seq snapshotSeq = 0;

        sequencer::SnapshotReader snapReader(snapshotDir_);
        if (auto snap = snapReader.loadNewestValid()) {
            for (const auto& rec : snap->orders) {
                NewOrder o{};
                o.id = rec.id;
                o.price = rec.price;
                o.quantity = rec.quantity;
                o.participant = rec.participant;
                o.side = static_cast<Side>(rec.side);
                o.type = OrderType::Limit;  // every resting order is, by construction, a LIMIT
                // A snapshot produced under the same maxOrders cap should never fail to restore
                // (pool exhaustion) or collide (duplicate id) -- either means the snapshot is
                // corrupt or the config drifted since it was written. Silently dropping the
                // order here would be a worse failure than a loud one (same principle as
                // restoreResting()'s own contract), so this is surfaced as Corrupt, never eaten.
                if (!book.restoreResting(o, rec.remaining, rec.seq)) {
                    result.status = RecoveryStatus::Corrupt;
                    return result;
                }
            }
            book.restoreCounters(static_cast<Seq>(snap->header.bookSeq),
                                 static_cast<Seq>(snap->header.nextTradeId));
            snapshotSeq = static_cast<Seq>(snap->header.globalSeq);
        }

        sequencer::JournalReader reader(journalDir_);
        Seq lastApplied = snapshotSeq;
        for (;;) {
            const sequencer::ReadResult r = reader.next();
            if (r.status == sequencer::ReadStatus::EndOfJournal) {
                break;
            }
            if (r.status == sequencer::ReadStatus::TruncatedTail) {
                result.status = RecoveryStatus::TornTail;
                break;
            }
            if (r.status == sequencer::ReadStatus::Corrupt) {
                result.status = RecoveryStatus::Corrupt;
                break;
            }
            if (r.globalSeq <= snapshotSeq) {
                continue;  // already covered by the snapshot's own state
            }
            applyRecord(book, r);
            lastApplied = r.globalSeq;
        }

        result.lastSeq = lastApplied;
        result.hasJournalSegment = reader.hasOpenSegment();
        if (result.hasJournalSegment) {
            result.resumeSegmentPath = reader.currentSegmentPath();
            result.resumeOffset = reader.currentOffset();
            result.resumeSegmentCreatedCounter = reader.currentSegmentCreatedCounter();
        }
        return result;
    }

 private:
    static void applyRecord(OrderBook& book, const sequencer::ReadResult& r) {
        Trade storage[64];
        TradeBuffer trades{storage, 64, 0};
        switch (r.kind) {
            case ipc::CommandKind::New: {
                const NewOrder o = ipc::toNewOrder(r.command);
                book.submit(o, trades);
                break;
            }
            case ipc::CommandKind::Cancel:
                book.cancel(r.command.id);
                break;
            case ipc::CommandKind::Replace: {
                NewOrder fresh = ipc::toNewOrder(r.command);
                fresh.id = r.command.newId;
                book.replace(r.command.id, fresh, trades);
                break;
            }
        }
    }

    std::filesystem::path journalDir_;
    std::filesystem::path snapshotDir_;
};

}  // namespace velox::recovery
