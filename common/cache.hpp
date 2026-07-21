#pragma once

#include <cstddef>

namespace velox {

// Cache-line size for this target. 64 bytes on every platform this project runs or benchmarks
// on (Apple Silicon, x86-64) -- there is no portable way to query it at compile time, so it is
// a constant, not a probe.
inline constexpr std::size_t kCacheLineSize = 64;

// Pads T out to a full cache line, so that two instances never share one -- the point being to
// stop false sharing between values that different threads write independently. Not used by
// the engine today (it is single-writer, so there is nothing to false-share yet); Spec 005's
// SPSC ring head/tail indices are the first consumer, where producer and consumer threads write
// adjacent cache-hot counters.
template<class T>
struct alignas(kCacheLineSize) CachePadded {
    T value;
};

static_assert(sizeof(CachePadded<int>) % kCacheLineSize == 0,
              "CachePadded must occupy a whole number of cache lines");

}  // namespace velox
