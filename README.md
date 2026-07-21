# velox — a low-latency order-matching engine

A single-threaded, deterministic, zero-allocation order-matching engine and mini-exchange in C++20.

**Status: Specs 001-004 complete (Phase A — Correct core, Phase B — Latency engineering).** The
engine handles the full order lifecycle (limit, market, IOC, FOK, cancel, cancel/replace,
self-trade prevention), matches by price-time priority, replays byte-identically, is proven
correct by a randomized invariant suite, and allocates **0 bytes per order** on the hot path. The
exchange surface (journal, gateway, market data, visualizer) is specified and queued — see
[`specs/`](specs/).

---

## What it is

The core of what a stock or crypto exchange runs. Buy and sell orders arrive; the engine maintains an
order book and matches them by **price-time priority** — best price first, then earliest arrival —
executing trades entirely in memory, optimized for the lowest and most *predictable* latency.

Around that core sits a thin but real exchange: a binary order gateway, a sequencer and journal that
make the system crash-recoverable and perfectly reproducible, a market-data feed, and a live
visualizer.

## The discipline

Two rules govern everything here, and they are enforced by tooling rather than by intention:

> **Every headline is a *measured* number on stated hardware — never "implemented X."**
> **Correctness is *proven* (replay + property + invariant tests), not claimed.**

The hot path (`engine/`, `book/`) may not allocate, lock, log, throw, or dispatch virtually. That is
checked by a git-committed hook on every file write and by a `latency-reviewer` sub-agent on every
edit. The performance budget is a build gate: a >20% p99 regression fails, exactly like a failing
test.

## Current measurements

Apple M4, macOS 26.5, Apple clang 21, `-O3 -DNDEBUG`, Release.
**No core isolation** — macOS provides none. See [`benchmarks/baselines/hardware.md`](benchmarks/baselines/hardware.md).

| | |
|---|---|
| Hot-path allocation | **0 bytes/op, 0 allocations/op** |
| Golden replay | **byte-identical** across 21 scenarios |
| Unit tests | 72 passing |
| Invariant (property) tests | 14 passing (randomized schedules, all profiles) |

Latency figures are in `benchmarks/baselines/summary.json`, and they come with caveats that travel
with them everywhere:

- They measure the **matching call in isolation** — no journal, no gateway, no ring buffer. This is
  not an end-to-end number and is not comparable to one.
- `steady_clock` on Apple Silicon has **~41 ns granularity**, which is *coarser than a single
  `submit()` call*. The harness therefore times batches of 64 and divides, and prints its own
  measured granularity so the limitation cannot quietly vanish from the output.
- The eventual ≥1M orders/sec throughput headline will be measured in `bench` mode, which **skips the
  journal** — making it a **no-durability number**. `fsync` costs tens of microseconds; a million per
  second is physically impossible on any storage device. That caveat is permanent and is stated
  everywhere the number appears.

## Build and run

```bash
cmake -B build -G Ninja          # deps auto-fetched (GoogleTest, Google Benchmark, HdrHistogram_c)
cmake --build build

ctest --test-dir build -L unit          # 72 unit + structural tests
ctest --test-dir build -L replay        # golden replay, byte-for-byte (21 scenarios)
ctest --test-dir build -L invariant     # randomized property tests (14 profiles)
ctest --test-dir build -L alloc_check   # must report 0 bytes/op
./build/benchmark/velox_bench           # p50/p99/p999
./build/benchmark/velox_alloc_check     # must report 0 bytes/op
```

Requires CMake ≥ 3.24, Ninja, and a C++20 compiler. Nothing else — no vcpkg, no Conan.

## The order book

```
BidLevels (price DESC)              AskLevels (price ASC)
  direct-indexed array                direct-indexed array
  price -> PriceLevel*                price -> PriceLevel*
  + bestBid tracked as a field        + bestAsk tracked as a field

     each PriceLevel = intrusive doubly-linked FIFO of Orders
     head ──> [Order] <──> [Order] <──> [Order] <── tail
             (earliest)                 (latest)

OrderIdMap: open-addressed id -> Order*   ← makes cancel O(1)
```

| Operation | Cost |
|---|---|
| Best bid / ask | O(1) — a tracked field |
| Insert | O(1) |
| **Cancel** | **O(1)** — id map finds it, intrusive links unlink it |
| Match | O(fills) |

**Why not a heap?** O(log n) best-price, but **O(n) cancel** — you cannot locate an arbitrary order.
Real order flow is dominated by cancels (often >90% of messages), so a heap optimizes the rare case at
the expense of the common one.

**Why not `std::map`?** A cache-hostile red-black tree with a node allocation per price level.

Prices are **scaled `int64_t`** (× 10,000). There is no floating point in the engine: floats bring
rounding error into money and non-determinism into comparison.

## Development

Spec-driven. The [`specs/`](specs/) backlog is a committed artifact — 001 through 011, each with
scope, a Definition of Done, and the requirements it satisfies. `plan.md` and `tasks.md` are written
when a spec is *picked up*, not in advance, because planning against an imagined codebase produces
fiction.

[`progress_report.md`](progress_report.md) is the append-only story of how this was built — including
the bugs. Three real ones were caught building Spec 001 alone, two of them in the benchmark
methodology itself (a benchmark that was measuring its own instrumentation, and one that was measuring
the *rejection* path after silently exhausting the order pool). They are written up rather than
quietly fixed, because how a system was debugged is more informative than the fact that it now works.

The [`.claude/`](.claude/) directory ships the guardrails: 9 slash commands, 4 domain skills, 4
sub-agents, and hooks that auto-format and lint the hot path on every write. They are committed so
they travel with the repo.
