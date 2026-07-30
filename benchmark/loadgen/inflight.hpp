#pragma once

// Spec 009 T1: the send -> complete correlation table.
//
// The generator thread and the reader thread are different threads, and the reader learns only
// an OrderId when a completion event arrives -- it needs to look up when that order was
// INTENDED to be sent and when it actually was, to feed latency_recorder.hpp's two spans.
//
// Three pre-sized arrays (not a hash map -- no heap growth, no rehashing pause), indexed
// `id & (kSlots - 1)`. Ids handed out by the generator are dense and monotonic, and every path's
// in+out ring pair holds at most ~2*65536 orders in flight at once, so a slot can never be reused
// by a still-in-flight order as long as kSlots exceeds the deepest possible backlog.
//
// The reader thread's completion callback ONLY writes into this table (recordComplete) -- it
// does NOT compute or record into a histogram inline. That separation matters: HdrHistogram's
// hdr_record_corrected_value() does O(span / interval) internal work per call, so calling it from
// the reader's hot drain loop means a single very-late sample (exactly the kind a stall
// produces) can block the reader for a long time, which delays every event behind it, which
// makes THEIR spans larger too -- an avalanche that starves the outbound ring and corrupts the
// very tail this harness exists to measure honestly. Recording is deferred to a single fast pass
// over this table after the run (see latency_recorder.hpp's recordRange()), fully decoupled from
// ring-draining speed.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace velox::loadgen {

class InflightTable {
 public:
    static constexpr std::size_t kSlots = 1 << 20;

    InflightTable() : intendedNs_(kSlots, 0), actualSendNs_(kSlots, 0), completeNs_(kSlots, -1) {}

    // Called by the generator thread immediately before/after sending order `id`.
    void recordSend(std::uint64_t id, std::int64_t intendedNs, std::int64_t actualSendNs) {
        const std::size_t slot = id & (kSlots - 1);
        intendedNs_[slot] = intendedNs;
        actualSendNs_[slot] = actualSendNs;
    }

    // Called by the reader thread the instant a completion event for `id` arrives. Deliberately
    // the ONLY thing the reader does per event -- see the file header for why.
    void recordComplete(std::uint64_t id, std::int64_t completeNs) {
        completeNs_[id & (kSlots - 1)] = completeNs;
    }

    std::int64_t intendedNs(std::uint64_t id) const { return intendedNs_[id & (kSlots - 1)]; }
    std::int64_t actualSendNs(std::uint64_t id) const { return actualSendNs_[id & (kSlots - 1)]; }
    std::int64_t completeNs(std::uint64_t id) const { return completeNs_[id & (kSlots - 1)]; }

 private:
    std::vector<std::int64_t> intendedNs_;
    std::vector<std::int64_t> actualSendNs_;
    std::vector<std::int64_t> completeNs_;
};

}  // namespace velox::loadgen
