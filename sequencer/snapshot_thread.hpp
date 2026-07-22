#pragma once

// The shadow-replay snapshot thread (Spec 006, spec "Conflict 1").
//
// `03-system-design.md` contradicted itself: §1.3 wants a dedicated thread with a deep copy,
// unpaused; §2.6 has the matching engine check an atomic flag and serialize ITS OWN state. The
// second is a direct hot-path violation -- serializing a 100k-order book inline would allocate
// and take I/O-shaped time, blowing the p99 budget by orders of magnitude. This class is the
// resolution: it owns its OWN OrderBook, completely independent of the live matching thread's,
// and replays the journal into it. Determinism (constitution P4) is what makes this sound: the
// shadow book at seq N is, by construction, identical to the live book at seq N. The matching
// thread's contribution to snapshotting is therefore not O(1) -- it is ZERO. No flag, no
// copy-out, no pause, ever.
//
// Simplification stated plainly: each snapshot replays the journal from the beginning rather
// than incrementally from the previous snapshot. Off the hot path, correctness-first, and the
// journal-tail-replay-distance bound (NFR-25) is still satisfied for RECOVERY (which only ever
// replays from the newest snapshot forward) -- this only affects how much work the shadow thread
// itself repeats, not recovery time.

#include <atomic>
#include <filesystem>
#include <thread>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "platform/platform.hpp"
#include "sequencer/journal_reader.hpp"
#include "sequencer/snapshot_writer.hpp"

namespace velox::sequencer {

class SnapshotThread {
 public:
    SnapshotThread(std::filesystem::path journalDir, std::filesystem::path snapshotDir,
                   BookConfig cfg)
        : journalDir_(std::move(journalDir)), snapshotDir_(std::move(snapshotDir)), cfg_(cfg) {}

    ~SnapshotThread() { stop(); }

    SnapshotThread(const SnapshotThread&) = delete;
    SnapshotThread& operator=(const SnapshotThread&) = delete;

    void start() {
        stop_.store(false, std::memory_order_relaxed);
        thread_ = std::thread([this] { run(); });
    }

    void stop() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    // Asynchronously request a snapshot covering the journal up to (and including) `targetSeq`.
    // Picked up by the background thread; a later request with a smaller seq than one already
    // serviced is a no-op.
    void requestSnapshot(Seq targetSeq) {
        requestedSeq_.store(targetSeq, std::memory_order_release);
    }

    std::size_t snapshotsWritten() const noexcept {
        return snapshotsWritten_.load(std::memory_order_acquire);
    }

    // Synchronous, single-shot form: replay the journal from scratch in a fresh shadow book up
    // to `targetSeq`, then write a snapshot. Used directly by tests asserting digest equality
    // without needing to synchronize with a background thread.
    bool snapshotNowSync(Seq targetSeq) {
        OrderBook shadow(cfg_);
        replayUpTo(shadow, targetSeq);
        SnapshotWriter writer(snapshotDir_);
        return writer.write(shadow, targetSeq, cfg_);
    }

 private:
    void run() {
        Seq lastDone = 0;
        while (!stop_.load(std::memory_order_acquire)) {
            const Seq target = requestedSeq_.load(std::memory_order_acquire);
            if (target > lastDone) {
                if (snapshotNowSync(target)) {
                    lastDone = target;
                    snapshotsWritten_.fetch_add(1, std::memory_order_release);
                }
            }
            platform::cpuPause();
        }
    }

    void replayUpTo(OrderBook& shadow, Seq targetSeq) {
        JournalReader reader(journalDir_);
        Trade storage[64];
        TradeBuffer trades{storage, 64, 0};
        for (;;) {
            const ReadResult r = reader.next();
            if (r.status != ReadStatus::Ok || r.globalSeq > targetSeq) {
                break;
            }
            trades.clear();
            switch (r.kind) {
                case ipc::CommandKind::New: {
                    const NewOrder o = ipc::toNewOrder(r.command);
                    shadow.submit(o, trades);
                    break;
                }
                case ipc::CommandKind::Cancel:
                    shadow.cancel(r.command.id);
                    break;
                case ipc::CommandKind::Replace: {
                    NewOrder fresh = ipc::toNewOrder(r.command);
                    fresh.id = r.command.newId;
                    shadow.replace(r.command.id, fresh, trades);
                    break;
                }
            }
        }
    }

    std::filesystem::path journalDir_;
    std::filesystem::path snapshotDir_;
    BookConfig cfg_;
    std::thread thread_;
    std::atomic<bool> stop_{true};
    std::atomic<Seq> requestedSeq_{0};
    std::atomic<std::size_t> snapshotsWritten_{0};
};

}  // namespace velox::sequencer
