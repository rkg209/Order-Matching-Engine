#pragma once

// Platform shim.
//
// The architecture assumes Linux-x86 (taskset, numactl, SCHED_FIFO, _mm_pause, huge pages).
// We develop and measure on macOS-arm64, where most of that does not exist. Rather than
// #ifdef-ing the engine, every platform difference is confined here so that engine/ and
// book/ read as pure algorithm.
//
// Where a capability is genuinely unavailable, the shim REPORTS THAT (returns false) rather
// than silently pretending. A benchmark that believes it got a pinned core when it did not
// produces a number that is a lie, and the lie is invisible.

#include <cstddef>

#if defined(__APPLE__)
#include <sys/mman.h>
#elif defined(__linux__)
#include <sched.h>
#include <sys/mman.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#endif

namespace velox::platform {

// Spin-wait hint. Tells the CPU we are in a busy-wait so it can de-pipeline and save power,
// and (on x86) avoids the memory-order violation penalty on loop exit.
inline void cpuPause() noexcept {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
    // arm64's equivalent. `yield` is the architectural spin-wait hint.
    __asm__ __volatile__("yield" ::: "memory");
#else
    // Unknown architecture: a compiler barrier is the best we can honestly do.
    __asm__ __volatile__("" ::: "memory");
#endif
}

// Pin the calling thread to a core.
//
// Returns TRUE only if the thread was actually pinned.
//
// macOS has no equivalent of sched_setaffinity. THREAD_AFFINITY_POLICY is only a hint to the
// scheduler about which threads share cache, and it is ignored entirely on Apple Silicon.
// So on macOS this returns FALSE — and callers must report that honestly in benchmark output
// rather than claiming an isolated core they did not get.
inline bool pinThreadToCpu([[maybe_unused]] int cpu) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
#else
    return false;  // Not supported. Say so; do not pretend.
#endif
}

// True when the platform can actually isolate a core for the matching thread.
inline constexpr bool supportsCoreIsolation() noexcept {
#if defined(__linux__)
    return true;
#else
    return false;
#endif
}

// Fault in and lock pages so the first order does not pay a page fault. A page fault on the
// hot path is a syscall in the middle of a match: invisible in the mean, brutal in the p999.
inline bool prefaultPages() noexcept {
#if defined(__linux__)
    return mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
#elif defined(__APPLE__)
    // mlockall exists but requires elevated privileges and is a no-op for our purposes here.
    // Warmup (touching every pooled object before measuring) is what actually faults pages in
    // on this platform -- see benchmark/velox_bench.cpp.
    return false;
#else
    return false;
#endif
}

inline const char* platformName() noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return "linux-x86_64";
#elif defined(__linux__) && defined(__aarch64__)
    return "linux-arm64";
#elif defined(__APPLE__) && defined(__aarch64__)
    return "macos-arm64";
#elif defined(__APPLE__)
    return "macos-x86_64";
#else
    return "unknown";
#endif
}

}  // namespace velox::platform
