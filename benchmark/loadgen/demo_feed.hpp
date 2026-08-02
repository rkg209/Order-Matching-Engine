#pragma once

// Spec 010 T3: wires velox_loadgen's outbound-ring consumer index 1 (non-gating -- see
// runtime/matching_thread.hpp's OutRing doc) to a market-data feed and a latency-stats feed, for
// --md-port/--stats-port. Shared by the normal rate-driven run() and --replay-journal mode
// (velox_loadgen.cpp), so the demo wiring exists exactly once.
//
// Deliberately does NOT wire a snapshot source/burst the way apps/velox_gateway.cpp does: the
// gateway can safely rebuild a snapshot off-thread because it replays from the JOURNAL, never the
// live OrderBook (constitution P2, single writer -- see the gateway's own comment on this). The
// loadgen's `engine` path keeps no journal, so reading its live OrderBook from this thread would
// be exactly the unsynchronized cross-thread read the gateway's design exists to avoid. A
// subscriber that connects to the loadgen's feed therefore sees only the LIVE delta stream from
// the moment it connects, never a backfilled snapshot of resting orders -- an accepted
// simplification for a load-generator demo feed, not a correctness gap in the engine.
//
// The live latency histogram (telemetry/live_latency.hpp) is recorded and published from the
// SAME thread on every call (onComplete()) -- that thread is whichever one drives the reader
// completion callback in velox_loadgen.cpp, i.e. the path's own reader thread. This matches
// LiveLatencyStats' single-writer contract exactly.

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <thread>

#include "marketdata/feed_server.hpp"
#include "marketdata/publisher.hpp"
#include "platform/platform.hpp"
#include "protocol/message_types.hpp"
#include "runtime/matching_thread.hpp"
#include "telemetry/live_latency.hpp"

namespace velox::loadgen {

class DemoFeed {
 public:
    using OutRing = runtime::MatchingThread<>::OutRing;

    DemoFeed(OutRing& ring, unsigned short mdPort, unsigned short statsPort,
             protocol::InstrumentId instrumentId = 1) {
        if (mdPort != 0) {
            mdServer_ = std::make_unique<marketdata::FeedServer>(io_);
            mdServer_->listen(mdPort);
            publisher_ = std::make_unique<marketdata::Publisher<OutRing>>(ring, instrumentId);
            publisher_->addSink(mdServer_.get());
        }
        if (statsPort != 0) {
            statsServer_ = std::make_unique<marketdata::FeedServer>(io_);
            statsServer_->listen(statsPort);
        }
        if (mdServer_ || statsServer_) {
            ioThread_ = std::thread([this] { io_.run(); });
        }
        if (publisher_) {
            pumpThread_ = std::thread([this] {
                while (!stop_.load(std::memory_order_acquire)) {
                    if (publisher_->pump() == 0) {
                        platform::cpuPause();
                    }
                }
            });
        }
        lastTick_ = std::chrono::steady_clock::now();
    }

    ~DemoFeed() {
        stop_.store(true, std::memory_order_release);
        if (pumpThread_.joinable()) pumpThread_.join();
        io_.stop();
        if (ioThread_.joinable()) ioThread_.join();
    }

    DemoFeed(const DemoFeed&) = delete;
    DemoFeed& operator=(const DemoFeed&) = delete;

    unsigned short mdPort() const { return mdServer_ ? mdServer_->localPort() : 0; }
    unsigned short statsPort() const { return statsServer_ ? statsServer_->localPort() : 0; }
    std::size_t mdSubscribers() const noexcept { return mdServer_ ? mdServer_->sessionCount() : 0; }

    // Blocks (bounded by `maxWait`) until the publisher's ring lag reaches zero, then gives the
    // io thread a short grace period to actually flush the writes that draining the ring
    // triggered. Call this BEFORE tearing a DemoFeed down (the destructor calls io_.stop(),
    // which abandons anything still queued in a FeedSession's outbound deque or not yet
    // dispatched onto the io_context) -- otherwise a demo/replay run can exit mid-flush, handing
    // a subscriber a truncated byte stream even though the engine-side data was never lost. This
    // is what makes --replay-journal's byte-identical claim (FR-40, tests/viz/
    // replay_determinism_test.cpp) hold in practice, not just "eventually consistent".
    void waitDrained(std::chrono::milliseconds maxWait = std::chrono::milliseconds(2000)) const {
        if (!publisher_) return;
        const auto deadline = std::chrono::steady_clock::now() + maxWait;
        while (publisher_->lag() != 0 && std::chrono::steady_clock::now() < deadline) {
            platform::cpuPause();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // Reader-thread only: feed a completed order's corrected span into the rolling histogram, and
    // -- at most once a second (NFR-31) -- publish + broadcast the current window as one JSON
    // line, then reset for the next window.
    void onComplete(std::int64_t correctedSpanNs, std::int64_t intervalNs) noexcept {
        if (!statsServer_) return;
        liveStats_.record(correctedSpanNs, intervalNs);
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - lastTick_).count();
        if (elapsed < 1.0) return;
        lastTick_ = now;
        liveStats_.publishAndReset(elapsed);
        char buf[256];
        const std::size_t n = telemetry::formatStatsLine(liveStats_.load(), buf, sizeof(buf));
        if (n > 0) {
            statsServer_->send(reinterpret_cast<const std::byte*>(buf), n);
        }
    }

 private:
    asio::io_context io_;
    std::unique_ptr<marketdata::FeedServer> mdServer_;
    std::unique_ptr<marketdata::FeedServer> statsServer_;
    std::unique_ptr<marketdata::Publisher<OutRing>> publisher_;
    std::thread ioThread_;
    std::thread pumpThread_;
    std::atomic<bool> stop_{false};
    telemetry::LiveLatencyStats liveStats_;
    std::chrono::steady_clock::time_point lastTick_;
};

}  // namespace velox::loadgen
