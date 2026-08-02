#pragma once

// Spec 010 T2: a live, rolling latency figure for the visualizer's histogram panel.
//
// benchmark/loadgen/latency_recorder.hpp's LatencyRecorder deliberately has NO cross-thread
// snapshot API: hdr_record_corrected_value() does O(span/interval) internal work per call, and a
// second thread reading percentiles out of that same histogram while the reader thread is mid-
// record would be a data race on hdr_histogram's internal counts array, not just a stale read.
// This is therefore a SEPARATE histogram -- LiveLatencyStats -- owned exclusively by the reader
// thread, fed the identical corrected span LatencyRecorder feeds, and reset once a second by that
// same thread. No other thread ever touches the histogram; every other thread reads only the
// published Snapshot, through a seqlock. This preserves the discipline documented in
// benchmark/loadgen/inflight.hpp's file header -- percentile computation never runs on, or blocks,
// the ring-draining path.

#include <hdr/hdr_histogram.h>

#include <atomic>
#include <cstdint>
#include <cstdio>

namespace velox::telemetry {

struct LatencySnapshot {
    std::int64_t p50Ns = 0;
    std::int64_t p99Ns = 0;
    std::int64_t p999Ns = 0;
    std::int64_t maxNs = 0;
    std::uint64_t count = 0;
    double rate = 0.0;
};

class LiveLatencyStats {
 public:
    LiveLatencyStats() { hdr_init(1, 10'000'000'000LL, 3, &hist_); }
    ~LiveLatencyStats() {
        if (hist_) hdr_close(hist_);
    }

    LiveLatencyStats(const LiveLatencyStats&) = delete;
    LiveLatencyStats& operator=(const LiveLatencyStats&) = delete;

    // Reader-thread only. `correctedSpanNs` is the SAME t_intended -> t_complete span
    // LatencyRecorder::record() feeds its own histogram with -- the live figure and the post-run
    // reported figure are the same corrected quantity, just windowed differently.
    void record(std::int64_t correctedSpanNs, std::int64_t intervalNs) noexcept {
        hdr_record_corrected_value(hist_, correctedSpanNs < 0 ? 0 : correctedSpanNs, intervalNs);
        ++windowCount_;
    }

    // Reader-thread only. Call ~once/sec (NFR-31): computes percentiles over the CURRENT window,
    // publishes them through the seqlock, then resets the histogram for the next window -- a
    // ROLLING figure, not a running one, so it is NOT byte-identical to the single post-run pass
    // benchmark/velox_loadgen.cpp reports (recorded as a caveat in hardware.md).
    void publishAndReset(double elapsedSecs) noexcept {
        LatencySnapshot snap;
        snap.p50Ns = static_cast<std::int64_t>(hdr_value_at_percentile(hist_, 50.0));
        snap.p99Ns = static_cast<std::int64_t>(hdr_value_at_percentile(hist_, 99.0));
        snap.p999Ns = static_cast<std::int64_t>(hdr_value_at_percentile(hist_, 99.9));
        snap.maxNs = static_cast<std::int64_t>(hdr_max(hist_));
        snap.count = windowCount_;
        snap.rate = elapsedSecs > 0.0 ? static_cast<double>(windowCount_) / elapsedSecs : 0.0;

        const std::uint64_t v = version_.load(std::memory_order_relaxed);
        version_.store(v + 1, std::memory_order_release);  // odd: writer in progress
        published_ = snap;
        version_.store(v + 2, std::memory_order_release);  // even: consistent again

        hdr_reset(hist_);
        windowCount_ = 0;
    }

    // Any thread. Seqlock read: retries if a publish was in flight or landed mid-read.
    LatencySnapshot load() const noexcept {
        LatencySnapshot out;
        for (;;) {
            const std::uint64_t v1 = version_.load(std::memory_order_acquire);
            if (v1 & 1) continue;  // writer in progress -- spin
            out = published_;
            const std::uint64_t v2 = version_.load(std::memory_order_acquire);
            if (v1 == v2) return out;
        }
    }

 private:
    hdr_histogram* hist_ = nullptr;
    std::uint64_t windowCount_ = 0;
    std::atomic<std::uint64_t> version_{0};
    LatencySnapshot published_{};
};

// One JSON line: {"t":"lat","p50_ns":...,"p99_ns":...,"p999_ns":...,"max_ns":...,"count":...,
// "rate":...,"corrected":true}\n -- returns the number of bytes written (excluding the null
// terminator), 0 if `cap` was too small to hold the line at all.
inline std::size_t formatStatsLine(const LatencySnapshot& s, char* dst, std::size_t cap) noexcept {
    const int n = std::snprintf(
        dst, cap,
        "{\"t\":\"lat\",\"p50_ns\":%lld,\"p99_ns\":%lld,\"p999_ns\":%lld,\"max_ns\":%lld,"
        "\"count\":%llu,\"rate\":%.1f,\"corrected\":true}\n",
        static_cast<long long>(s.p50Ns), static_cast<long long>(s.p99Ns),
        static_cast<long long>(s.p999Ns), static_cast<long long>(s.maxNs),
        static_cast<unsigned long long>(s.count), s.rate);
    if (n < 0 || static_cast<std::size_t>(n) >= cap) return 0;
    return static_cast<std::size_t>(n);
}

}  // namespace velox::telemetry
