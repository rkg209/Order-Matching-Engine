# Spec 001 — Core limit order book + price-time matching

**Status:** ✅ COMPLETE (2026-07-14) · **Phase:** A — Correct core · **Depends on:** 000

> **Outcome:** 54/54 tests green. 7 golden scenarios replay byte-identically. Hot path allocates
> **0 bytes/op**. Measured p50 **11 ns** / p99 **14 ns** / p999 **18 ns** for the matching call in
> isolation on an Apple M4 with no core isolation — cross-validated against Google Benchmark
> (11.7 ns/op). Four real bugs found and fixed along the way, three of them in the measurement
> apparatus rather than the engine. See `progress_report.md` [005].

## Scope

The thin vertical slice: a single-instrument, in-memory limit order book that matches by **price-time
priority**, emits trades, and rests residual quantity — with a latency number printed from day one.

This is the smallest thing that is recognizably a matching engine, and it must be *demoable
end-to-end* before Spec 002 begins (constitution Principle 5).

## Non-goals (deliberately deferred — do not build these here)

| Deferred | To |
|---|---|
| Market, IOC, FOK orders; cancel; cancel/replace; self-trade prevention | Spec 002 |
| Property/invariant test harness over randomized schedules | Spec 003 |
| Object pooling and the zero-allocation push | Spec 004 |
| The SPSC ring buffer and threading | Spec 005 |
| Sequencer, journal, recovery | Spec 006 |
| Gateway, wire protocol, market data, visualizer | Specs 007–010 |

Keeping this list honest is the point of the slice. **Limit orders only.**

## Behavior

**Submit a limit order.** It matches against the opposite side of the book while it crosses, emitting
a trade per fill; whatever quantity remains then rests in the book at its limit price.

- **Crossing:** a buy crosses when `price >= bestAsk`; a sell crosses when `price <= bestBid`.
- **Priority:** best price first; ties broken by **earliest arrival** (strict FIFO within a level).
- **Trade price:** the **resting** order's price, never the aggressor's. A buy at 101 hitting a
  resting sell at 100 trades at **100** — the resting order set the terms.
- **Partial fills:** an aggressor may produce many trades. A resting order that is partially filled
  **keeps its queue position**.
- **Residual:** rests at the order's limit price. A fully-filled aggressor rests nothing.
- **Empty book:** an order with no opposite side simply rests. No crash, no trade at a sentinel price.

Prices are **scaled `int64_t`** (price × 10,000). No floating point.

## Definition of Done

- [x] A known order sequence replays to a **byte-identical** golden trade output (FR-47).
- [x] The golden scenarios for this slice pass: **limit match · partial fill · full fill · crossing
      book · empty book · FIFO within a level · price improvement**.
- [x] Unit tests cover the price level (FIFO enqueue/unlink), the level maps (best-price tracking,
      sentinels), and the id map.
- [x] `./build/benchmark/velox_bench` prints a **real** p50/p99/p999 for the matching call, measured
      with HdrHistogram. The number is *recorded*, not assumed — even if it is bad. A bad first number
      is fine; an unknown one is not.
- [x] `benchmarks/baselines/hardware.md` states the actual machine, and says plainly that macOS-arm64
      offers **no core isolation**.
- [x] `progress_report.md` has an entry with the real measured numbers.

## Requirements satisfied

| Req | How |
|---|---|
| **FR-1** | In-memory book, bids DESC / asks ASC |
| **FR-2** | Per-price-level FIFO as an intrusive doubly-linked list |
| **FR-4** | Match by price-time priority |
| **FR-5** | Limit orders; residual rests |
| **FR-11** | Partial fills; the resting remainder keeps its queue position |
| **FR-12** | Crossing books: a resting limit that would cross matches instead of resting |
| **FR-15** | A trade tick per matched pair |
| **FR-47** | Golden replay, compared byte-for-byte |
| **Principle 6** | A latency harness exists from this spec, before there is anything to optimize |

FR-3 (the id map, for O(1) cancel) is **built here but not yet exercised** — cancel arrives in Spec
002. Building the map now costs nothing and avoids retrofitting the data structure later.

## Verification

```bash
cmake -B build -G Ninja && cmake --build build
ctest --test-dir build -L unit   --output-on-failure
ctest --test-dir build -L replay --output-on-failure      # byte-identical
./build/benchmark/velox_bench                             # real p50/p99/p999
./build/benchmark/velox_alloc_check                       # baseline bytes/op (0 is the target,
                                                          # but Spec 004 is what enforces it)
```

## Known trap

`planning/03-system-design.md`'s matching-loop pseudocode **has a side bug** — it rests into the bid
book and removes from the ask book with no branch on `order.side`, so a resting SELL would land in the
bids. **Do not copy it.** Write the loop from the semantics above and from the `matching-semantics`
skill.
