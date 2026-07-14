# Benchmark hardware

> Per DR-8 and NFR-5: **a latency number without its hardware context is not a result.** Every
> figure this project publishes must be traceable to this file.

## Current development machine (where the committed baseline was measured)

| | |
|---|---|
| **CPU** | Apple M4 |
| **Cores** | 10 logical (4 performance + 6 efficiency) |
| **RAM** | 16 GB unified |
| **OS** | macOS 26.5.2 (Darwin 25.5.0), arm64 |
| **Compiler** | Apple clang 21.0.0 (clang-2100.1.1.101) |
| **Build flags** | `-O3 -g -DNDEBUG -fno-omit-frame-pointer`, C++20 |
| **Build type** | Release |

## Limitations of this machine — read before quoting any number from it

These are stated plainly rather than buried, because a benchmark's caveats are part of its result.

**1. There is NO core isolation. None.**
macOS provides no equivalent of `taskset`, `numactl`, `sched_setaffinity`, or `SCHED_FIFO`.
`THREAD_AFFINITY_POLICY` is a cache-sharing *hint* and is ignored outright on Apple Silicon. So
`platform::pinThreadToCpu()` returns **false** here, and the matching thread runs on a core shared
with the rest of the OS.

Consequence: **the tail (p99, p999, max) includes OS scheduling noise that a properly isolated Linux
box would not have.** The tail measured here is a pessimistic bound, not the engine's true tail.

**2. The scheduler may migrate the thread between P-cores and E-cores.**
The M4 is heterogeneous. A thread moved onto an efficiency core runs materially slower. We cannot
prevent this, and it is a plausible source of tail outliers.

**3. `steady_clock` granularity is ~41 ns.**
It is backed by a ~24 MHz timebase. **This is coarser than a single `submit()` call**, which costs
roughly 5–6 ns. Timing one call with two clock reads therefore measures the *clock*, not the engine:
it returns 0 (both reads in the same tick) or a multiple of ~41.67 ns.

The harness handles this by timing **batches of 64 orders** and dividing, so each sample is well
above the tick. It also probes and prints the granularity at runtime, so the limitation can never
quietly disappear from the output. See `progress_report.md` [005].

**4. Frequency scaling and turbo cannot be disabled.**
No `cpupower`, no way to pin the clock. Run-to-run variance is higher than on a tuned Linux host.

**5. Do NOT benchmark in Docker on this machine.**
Docker on Apple Silicon runs a Linux VM. Latency numbers measured inside it are worthless and must
never be published.

## The Linux target (for the eventual headline numbers)

When a Linux x86_64 box is available, re-measure there with the full isolation the architecture
assumes — `isolcpus`, `taskset`/`numactl` pinning, `SCHED_FIFO`, IRQs moved off the matching core,
frequency governor set to `performance`, turbo disabled, huge pages, `mlockall`. The `platform/`
shim already implements all of it behind `#ifdef __linux__`.

Publish **both** sets of numbers, clearly labeled. The macOS numbers are honest numbers on a shared
core; the Linux numbers will be the tuned ones. Presenting the tuned figure while having measured the
shared one is the exact dishonesty this project is built to avoid.
