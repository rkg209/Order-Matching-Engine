#pragma once

// The threading model around the ring (Spec 005).
//
// This file owns std::thread, std::atomic<bool>, and the OrderBook itself -- it is lifecycle,
// not the inner algorithm, so it is deliberately OUTSIDE the hot-path structural gate (unlike
// ipc/, which is covered). The drain loop it runs is: tryPeek -> dispatch -> publish outbound
// -> consume; on empty, platform::cpuPause().
//
// Outbound design decision (Spec 005 T4, measured, not assumed): the outbound ring is a
// MulticastRing<OutboundEvent, 2> -- one write per event, gated on the min of the two consumer
// cursors (the execution-report router and the market-data publisher, Spec 007/008). The
// alternative -- two independent SpscRing<OutboundEvent>, dual-published -- was measured
// head-to-head in benchmark/velox_ring_bench.cpp and lost by ~3x on both throughput and p99
// (dual publish pays two full claim/publish sequences per event; the min-gate costs one branch
// over N=2 cursors). See specs/005-spsc-ring-ingress/plan.md for the numbers. The losing design
// is not implemented here at all -- there is no parallel "two-ring" code path to delete later.

#include <atomic>
#include <thread>

#include "engine/order_book.hpp"
#include "engine/trade.hpp"
#include "ipc/command.hpp"
#include "ipc/multicast_ring.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "platform/platform.hpp"

namespace velox::runtime {

// Bundles the inbound and outbound rings this thread reads/writes, so callers do not have to
// wire up three template parameters by hand at every call site.
template<std::size_t InCapacity = 65536, std::size_t OutCapacity = 65536>
class MatchingThread {
 public:
    using InRing = ipc::SpscRing<ipc::Command, InCapacity>;
    using OutRing = ipc::MulticastRing<ipc::OutboundEvent, 2, OutCapacity>;

    MatchingThread(InRing& in, OutRing& out, const BookConfig& cfg, int cpu = 0)
        : in_(in), out_(out), book_(cfg), cpu_(cpu) {}

    ~MatchingThread() { stop(); }

    // Records whether the OS actually honoured the pin request. On macOS-arm64 this is always
    // false -- platform::pinThreadToCpu() reports that honestly, and so does this.
    bool pinned() const noexcept { return pinned_.load(std::memory_order_acquire); }

    std::size_t fullSpins() const noexcept { return fullSpins_.load(std::memory_order_relaxed); }

    // Count of inbound commands fully dispatched (all their outbound events already published).
    // Test-only instrumentation: it lets a test driver know precisely when it is safe to drain
    // the outbound ring for a given command without racing the matching thread (see
    // tests/replay/replay_test.cpp's through-the-ring suite).
    std::size_t processedCount() const noexcept {
        return processedCount_.load(std::memory_order_acquire);
    }

    void start() {
        stop_.store(false, std::memory_order_relaxed);
        thread_ = std::thread([this] { run(); });
    }

    // Signals shutdown; the loop only checks stop_ when the ring is EMPTY, so no command
    // sitting in the ring at shutdown time is ever abandoned.
    void stop() {
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    const OrderBook& book() const noexcept { return book_; }

    // Recovery restore hook (Spec 006). Lets a caller (e.g. RecoveryManager) rebuild this
    // thread's OrderBook from a snapshot/journal tail BEFORE start() spawns the matching thread.
    // MUST be called before start() -- calling it after is undefined, since the matching thread
    // would then be concurrently reading/writing the same book. This is not a hot-path method:
    // it runs once, synchronously, on the calling thread, before there IS a matching thread, so
    // it cannot perturb the dispatch loop's codegen.
    template<class F>
    void restoreBeforeStart(F&& f) {
        f(book_);
    }

    // Spec 007: dispatchSeq_ must start where the recovered journal left off, or a gateway
    // restarting mid-stream would stamp OutboundEvents with sequence numbers that collide with
    // (or gap behind) ones already durable from a previous run. MUST be called before start(),
    // same rule as restoreBeforeStart().
    void restoreDispatchSeq(Seq lastSeq) noexcept { dispatchSeq_ = lastSeq; }

 private:
    void run() {
        pinned_.store(platform::pinThreadToCpu(cpu_), std::memory_order_release);

        Trade storage[64];
        TradeBuffer buf{storage, 64, 0};

        for (;;) {
            const ipc::Command* cmd = in_.tryPeek();
            if (cmd == nullptr) {
                if (stop_.load(std::memory_order_acquire)) {
                    return;
                }
                platform::cpuPause();
                continue;
            }

            dispatch(*cmd, buf);
            in_.consume();
            processedCount_.fetch_add(1, std::memory_order_release);
        }
    }

    void dispatch(const ipc::Command& cmd, TradeBuffer& buf) {
        // Deterministic (constitution P4): derived purely from ring arrival order, one
        // increment per dispatch() call, never from a clock.
        ++dispatchSeq_;
        buf.clear();
        switch (cmd.kind) {
            case ipc::CommandKind::New: {
                const NewOrder o = ipc::toNewOrder(cmd);
                const SubmitStatus st = book_.submit(o, buf);
                publishTrades(buf);
                if (st != SubmitStatus::Ok) {
                    publishOutbound(ipc::statusEvent(cmd.id, st, dispatchSeq_));
                }
                break;
            }
            case ipc::CommandKind::Cancel: {
                const OrderResult r = book_.cancel(cmd.id);
                publishOutbound(ipc::statusEvent(cmd.id, r.status, dispatchSeq_));
                break;
            }
            case ipc::CommandKind::Replace: {
                // toNewOrder() cannot be used here: it maps Command::id into NewOrder::id, but
                // for Replace, Command::id is the OLD id (see ipc/command.hpp) -- the fresh
                // order's id is newId.
                NewOrder fresh = ipc::toNewOrder(cmd);
                fresh.id = cmd.newId;
                const OrderResult r = book_.replace(cmd.id, fresh, buf);
                publishTrades(buf);
                publishOutbound(ipc::statusEvent(cmd.newId, r.status, dispatchSeq_));
                break;
            }
        }
    }

    void publishTrades(TradeBuffer& buf) {
        for (std::size_t i = 0; i < buf.count && i < buf.capacity; ++i) {
            publishOutbound(ipc::tradeEvent(buf.data[i], dispatchSeq_));
        }
    }

    // Spins rather than drops (FR-28): a full outbound ring is backpressure, observable via
    // fullSpins_, never a silently discarded event. One claim/publish per event -- this is
    // exactly the write MulticastRing was chosen to avoid doubling (see the design note above).
    void publishOutbound(const ipc::OutboundEvent& e) {
        ipc::OutboundEvent* slot = out_.tryClaim();
        while (slot == nullptr) {
            fullSpins_.fetch_add(1, std::memory_order_relaxed);
            platform::cpuPause();
            slot = out_.tryClaim();
        }
        *slot = e;
        out_.publish();
    }

    InRing& in_;
    OutRing& out_;
    OrderBook book_;
    int cpu_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> pinned_{false};
    alignas(64) std::atomic<std::size_t> fullSpins_{0};
    alignas(64) std::atomic<std::size_t> processedCount_{0};
    Seq dispatchSeq_ = 0;  // matching-thread-only, never touched cross-thread -- no atomic needed
};

}  // namespace velox::runtime
