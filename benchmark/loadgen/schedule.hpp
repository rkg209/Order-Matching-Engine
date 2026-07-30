#pragma once

// Spec 009 T1: the intended schedule -- the instrument that makes rate-driven measurement
// honest. See .claude/skills/benchmark-methodology/SKILL.md for the full coordinated-omission
// argument; the short version lives here because the code must match it exactly.
//
// intended(i) is computed ABSOLUTELY from t0, never accumulated from the previous actual send.
// Accumulating is drift: if the generator falls behind, "intended += interval" silently slides
// the whole schedule later and the run quietly turns into a saturation test wearing a rate
// label. Absolute-from-t0 means a late send does not push later sends later -- it just means the
// generator is behind, which is exactly the condition hdr_record_corrected_value() needs to see.

#include <chrono>
#include <cstdint>

#include "platform/platform.hpp"

namespace velox::loadgen {

using Clock = std::chrono::steady_clock;

class Schedule {
 public:
    Schedule(Clock::time_point t0, std::int64_t intervalNs) : t0_(t0), intervalNs_(intervalNs) {}

    // Nanoseconds since t0 at which send index i was SUPPOSED to happen, computed fresh every
    // call -- never from a running accumulator.
    std::int64_t intendedNs(std::uint64_t i) const noexcept {
        return static_cast<std::int64_t>(i) * intervalNs_;
    }

    Clock::time_point intendedTime(std::uint64_t i) const noexcept {
        return t0_ + std::chrono::nanoseconds(intendedNs(i));
    }

    std::int64_t intervalNs() const noexcept { return intervalNs_; }
    Clock::time_point t0() const noexcept { return t0_; }

    // Spins until `target`, using platform::cpuPause() rather than sleep_for -- sleep_for has
    // ~ms granularity on macOS, which cannot pace a schedule with a microsecond-scale interval.
    // If `target` is already in the past, returns immediately (send now, do not spin backwards).
    static void waitUntil(Clock::time_point target) noexcept {
        while (Clock::now() < target) {
            platform::cpuPause();
        }
    }

 private:
    Clock::time_point t0_;
    std::int64_t intervalNs_;
};

}  // namespace velox::loadgen
