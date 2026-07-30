#pragma once

// Spec 009 T1/decision 2: the naive/corrected histogram pair.
//
// Per order the generator stores two timestamps (see inflight.hpp) and this class computes two
// DELTAS from them -- not two record calls sharing one start time, which would make the whole
// coordinated-omission demonstration theatre:
//
//   naive:     t_actual_send -> t_complete, via hdr_record_value(). The WRONG way: a stalled
//              generator simply never sends the orders that would show the tail, so this span
//              looks great precisely when the system is struggling.
//   corrected: t_intended  -> t_complete, via hdr_record_corrected_value(hist, v, intervalNs).
//              hdr_record_corrected_value backfills the phantom samples the naive loop would
//              have produced had it kept sending on schedule, so a stall shows up in the tail.
//
// Both share the 1 ns .. 10 s / 3-sig-fig configuration velox_bench.cpp already established.

#include <hdr/hdr_histogram.h>

#include <cstdint>
#include <cstdio>

#include "loadgen/inflight.hpp"

namespace velox::loadgen {

class LatencyRecorder {
 public:
    LatencyRecorder() {
        hdr_init(1, 10'000'000'000LL, 3, &corrected_);
        hdr_init(1, 10'000'000'000LL, 3, &naive_);
    }

    ~LatencyRecorder() {
        if (corrected_) hdr_close(corrected_);
        if (naive_) hdr_close(naive_);
    }

    LatencyRecorder(const LatencyRecorder&) = delete;
    LatencyRecorder& operator=(const LatencyRecorder&) = delete;

    // Called once per completed order. intendedNs/actualSendNs/completeNs are all relative to
    // the same t0 (nanosecond offsets), so the subtractions below are plain deltas.
    void record(std::int64_t intendedNs, std::int64_t actualSendNs, std::int64_t completeNs,
                std::int64_t intervalNs) {
        const std::int64_t naiveSpan = completeNs - actualSendNs;
        const std::int64_t correctedSpan = completeNs - intendedNs;
        hdr_record_value(naive_, naiveSpan < 0 ? 0 : naiveSpan);
        hdr_record_corrected_value(corrected_, correctedSpan < 0 ? 0 : correctedSpan, intervalNs);
    }

    hdr_histogram* corrected() noexcept { return corrected_; }
    hdr_histogram* naive() noexcept { return naive_; }

    static long long percentile(hdr_histogram* h, double p) {
        return static_cast<long long>(hdr_value_at_percentile(h, p));
    }

 private:
    hdr_histogram* corrected_ = nullptr;
    hdr_histogram* naive_ = nullptr;
};

// Records `count` orders, ids [firstId, firstId + count), from `table` into `recorder`, in one
// fast sequential pass. Deliberately run AFTER the reader thread has finished draining -- never
// interleaved with it -- so hdr_record_corrected_value()'s per-call cost (O(span / intervalNs))
// can never compete with ring-draining speed. See inflight.hpp's file header for the avalanche
// this decoupling exists to prevent: a single very-late sample recorded inline on the reader's
// hot path delays every event behind it, which makes their spans larger too.
inline void recordRange(LatencyRecorder& recorder, const InflightTable& table,
                        std::uint64_t firstId, std::size_t count, std::int64_t intervalNs) {
    for (std::size_t i = 0; i < count; ++i) {
        const std::uint64_t id = firstId + i;
        recorder.record(table.intendedNs(id), table.actualSendNs(id), table.completeNs(id),
                        intervalNs);
    }
}

}  // namespace velox::loadgen
