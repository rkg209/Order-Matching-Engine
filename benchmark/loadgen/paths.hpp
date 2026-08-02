#pragma once

// Spec 009 T2: the three measured paths. Each exposes the same shape -- construct, populate(),
// sendOne(), startReader(onComplete)/stopReader() -- so benchmark/velox_loadgen.cpp can drive
// whichever one --path= selected without caring which it got.
//
// Completion marker (engine/durable): MatchingThread::dispatch() publishes exactly one
// OutboundKind::OrderUpdate carrying a New command's id per dispatch -- Rested if it joined the
// book, Rejected otherwise (runtime/matching_thread.hpp:~149). That is the terminal, one-per-
// order marker these two paths key on.
//
// Completion marker (wire): the first EXEC_REPORT/REJECT frame for the id. NOTE, stated plainly
// because it is a real asymmetry with engine/durable: ClientSession::sendNewAckIfApplicable()
// sends a NEW_ACK synchronously the instant the sequencer durably assigns a sequence number
// (gateway/session.cpp), which is BEFORE the matching thread has necessarily dispatched the
// command at all. So the wire path's figure is sequencer-round-trip-and-TCP dominated, not a
// superset of order-to-match the way the plan's general caveat assumes -- it can UNDERSTATE
// order-to-match for a resting order. This is a structural property of the wire protocol (the
// hot path deliberately does not publish a StatusEvent for a successful New -- see
// runtime/matching_thread.hpp), not a bug in this harness. State it every time wire numbers are
// reported.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/multicast_ring.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "loadgen/wire_harness.hpp"
#include "loadgen/workload.hpp"
#include "platform/platform.hpp"
#include "protocol/decoder.hpp"
#include "protocol/messages.hpp"
#include "runtime/matching_thread.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/sequencer.hpp"

namespace velox::loadgen {

using Clock = std::chrono::steady_clock;

inline std::int64_t nsSince(Clock::time_point t0) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count();
}

// --- engine: SpscRing -> MatchingThread -> MulticastRing cons. 0, no journal, no TCP ----------
class EnginePath {
 public:
    static constexpr const char* kName = "engine";
    static constexpr bool kDurable = false;
    static constexpr bool kWire = false;
    using OutRing = runtime::MatchingThread<>::OutRing;

    EnginePath() : matching_(inRing_, outRing_, makeConfig()) { matching_.start(); }
    ~EnginePath() {
        stopReader();
        matching_.stop();
    }

    void sendOne(const ipc::Command& cmd) {
        while (!inRing_.push(cmd)) {
            platform::cpuPause();
        }
    }

    void waitProcessed(std::size_t n) {
        while (matching_.processedCount() < n) {
            platform::cpuPause();
        }
    }

    template<class OnComplete>
    void startReader(OnComplete onComplete, Clock::time_point t0) {
        readerStop_.store(false, std::memory_order_relaxed);
        readerThread_ = std::thread([this, onComplete, t0] { runReader(onComplete, t0); });
    }

    void stopReader() {
        readerStop_.store(true, std::memory_order_release);
        if (readerThread_.joinable()) readerThread_.join();
    }

    bool pinned() const { return matching_.pinned(); }
    std::size_t fullSpins() const { return matching_.fullSpins(); }
    std::uint64_t lappedCount() const { return outRing_.lappedCount(0); }
    std::size_t poolExhaustedRejects() const {
        return poolExhaustedRejects_.load(std::memory_order_relaxed);
    }

    // Spec 010 T3: the mechanical basis of the visualizer's decoupling claim. Consumer index 1
    // is non-gating (bit clear in OutRing's GatingMask -- runtime/matching_thread.hpp), so a
    // market-data publisher attached here can lap or not exist at all without ever stalling the
    // matching thread reading via consumer 0 above.
    OutRing& outRing() noexcept { return outRing_; }

 private:
    template<class OnComplete>
    void runReader(OnComplete& onComplete, Clock::time_point t0) {
        const ipc::OutboundEvent* ev;
        for (;;) {
            ev = outRing_.tryPeek(0);
            if (ev == nullptr) {
                if (readerStop_.load(std::memory_order_acquire)) break;
                platform::cpuPause();
                continue;
            }
            handle(*ev, onComplete, t0);
            outRing_.consume(0);
        }
        while ((ev = outRing_.tryPeek(0)) != nullptr) {
            handle(*ev, onComplete, t0);
            outRing_.consume(0);
        }
    }

    template<class OnComplete>
    void handle(const ipc::OutboundEvent& ev, OnComplete& onComplete, Clock::time_point t0) {
        if (ev.kind != ipc::OutboundKind::OrderUpdate) return;
        const std::int64_t completeNs = nsSince(t0);
        if (ev.action == ipc::UpdateAction::Rejected &&
            ev.status == SubmitStatus::RejectedPoolExhausted) {
            poolExhaustedRejects_.fetch_add(1, std::memory_order_relaxed);
        }
        onComplete(ev.payload.orderUpdate.orderId, completeNs);
    }

 private:
    ipc::SpscRing<ipc::Command> inRing_;
    OutRing outRing_;
    runtime::MatchingThread<> matching_;
    std::thread readerThread_;
    std::atomic<bool> readerStop_{false};
    std::atomic<std::size_t> poolExhaustedRejects_{0};
};

// --- durable: as engine, plus Sequencer + JournalWriter fsync before the ring push -------------
class DurablePath {
 public:
    static constexpr const char* kName = "durable";
    static constexpr bool kDurable = true;
    static constexpr bool kWire = false;

    explicit DurablePath(std::size_t groupCommit)
        : matching_(inRing_, outRing_, makeConfig()),
          journalDir_(makeLoadgenTempDir("durable")),
          journal_(
              journalDir_, 256u * 1024 * 1024,
              groupCommit > 0 ? sequencer::FsyncPolicy::Group : sequencer::FsyncPolicy::PerRecord,
              groupCommit > 0 ? groupCommit : 1),
          seqr_(journal_, inRing_, 0) {
        matching_.start();
    }

    ~DurablePath() {
        stopReader();
        matching_.stop();
    }

    void sendOne(const ipc::Command& cmd) { seqr_.submit(cmd.kind, cmd); }

    void waitProcessed(std::size_t n) {
        while (matching_.processedCount() < n) {
            platform::cpuPause();
        }
    }

    template<class OnComplete>
    void startReader(OnComplete onComplete, Clock::time_point t0) {
        readerStop_.store(false, std::memory_order_relaxed);
        readerThread_ = std::thread([this, onComplete, t0] { runReader(onComplete, t0); });
    }

    void stopReader() {
        readerStop_.store(true, std::memory_order_release);
        if (readerThread_.joinable()) readerThread_.join();
    }

    bool pinned() const { return matching_.pinned(); }
    std::size_t fullSpins() const { return matching_.fullSpins(); }
    std::uint64_t lappedCount() const { return outRing_.lappedCount(0); }
    std::size_t poolExhaustedRejects() const {
        return poolExhaustedRejects_.load(std::memory_order_relaxed);
    }

 private:
    template<class OnComplete>
    void runReader(OnComplete& onComplete, Clock::time_point t0) {
        const ipc::OutboundEvent* ev;
        for (;;) {
            ev = outRing_.tryPeek(0);
            if (ev == nullptr) {
                if (readerStop_.load(std::memory_order_acquire)) break;
                platform::cpuPause();
                continue;
            }
            handle(*ev, onComplete, t0);
            outRing_.consume(0);
        }
        while ((ev = outRing_.tryPeek(0)) != nullptr) {
            handle(*ev, onComplete, t0);
            outRing_.consume(0);
        }
    }

    template<class OnComplete>
    void handle(const ipc::OutboundEvent& ev, OnComplete& onComplete, Clock::time_point t0) {
        if (ev.kind != ipc::OutboundKind::OrderUpdate) return;
        const std::int64_t completeNs = nsSince(t0);
        if (ev.action == ipc::UpdateAction::Rejected &&
            ev.status == SubmitStatus::RejectedPoolExhausted) {
            poolExhaustedRejects_.fetch_add(1, std::memory_order_relaxed);
        }
        onComplete(ev.payload.orderUpdate.orderId, completeNs);
    }

    ipc::SpscRing<ipc::Command> inRing_;
    runtime::MatchingThread<>::OutRing outRing_;
    runtime::MatchingThread<> matching_;
    std::filesystem::path journalDir_;
    sequencer::JournalWriter journal_;
    sequencer::Sequencer<ipc::SpscRing<ipc::Command>> seqr_;
    std::thread readerThread_;
    std::atomic<bool> readerStop_{false};
    std::atomic<std::size_t> poolExhaustedRejects_{0};
};

// --- wire: real GatewayServer + real wire protocol over loopback TCP --------------------------
class WirePath {
 public:
    static constexpr const char* kName = "wire";
    static constexpr bool kDurable = true;
    static constexpr bool kWire = true;

    // Logs in as participant 2 -- the "cross" side of workload.hpp's steady-state pair; the
    // gateway routes exec reports/rejects for an order back to whichever session submitted it
    // (gateway/gateway.hpp's routes_ map keyed by orderId), so a single connection can submit
    // both legs of the pair and see both outcomes on the one socket it reads back on.
    WirePath() : harness_(), client_(harness_.port) {
        unsigned char token[32];
        makeLoadgenToken(2, token);
        if (!client_.login(2, token)) {
            std::fprintf(stderr, "velox_loadgen: wire path login failed -- aborting\n");
            std::abort();
        }
    }

    void sendOne(const ipc::Command& cmd) {
        if (!client_.sendNewOrder(nextClientSeq_++, cmd.id, cmd.side, cmd.price, cmd.quantity)) {
            std::fprintf(stderr,
                         "velox_loadgen: wire path write failed (connection closed) -- aborting\n");
            std::abort();
        }
    }

    void waitProcessed(std::size_t) {
        // No ring-side counter to poll on the wire path; the caller paces via the reader thread
        // observing completions instead (see velox_loadgen.cpp's populate step).
    }

    template<class OnComplete>
    void startReader(OnComplete onComplete, Clock::time_point t0) {
        readerStop_.store(false, std::memory_order_relaxed);
        readerThread_ = std::thread([this, onComplete, t0] { runReader(onComplete, t0); });
    }

    void stopReader() {
        readerStop_.store(true, std::memory_order_release);
        if (readerThread_.joinable()) {
            client_.close();  // unblock a reader stuck in a blocking read
            readerThread_.join();
        }
    }

    std::size_t poolExhaustedRejects() const { return 0; }  // TCP path never sees this directly

 private:
    template<class OnComplete>
    void runReader(OnComplete& onComplete, Clock::time_point t0) {
        protocol::DecodedMessage msg;
        while (!readerStop_.load(std::memory_order_acquire)) {
            if (!client_.readOne(msg)) {
                if (readerStop_.load(std::memory_order_acquire)) break;
                continue;
            }
            const std::int64_t completeNs = nsSince(t0);
            if (msg.type == protocol::MessageType::ExecReport) {
                onComplete(msg.execReport.orderId, completeNs);
            } else if (msg.type == protocol::MessageType::Reject) {
                onComplete(msg.reject.orderId, completeNs);
            }
        }
    }

    WireHarness harness_;
    LoadgenClient client_;
    std::uint64_t nextClientSeq_ = 1;
    std::thread readerThread_;
    std::atomic<bool> readerStop_{false};
};

}  // namespace velox::loadgen
