# Spec 001 — Tasks

Every behavior-adding task names the test that proves it (NFR-37). A task with no test is not a task.

## Phase 1 — Build skeleton

- [x] **T1.1** Top-level `CMakeLists.txt`: C++20, Release/Debug, Ninja, `FetchContent` for GoogleTest,
      Google Benchmark, HdrHistogram_c. CTest labels: `unit`, `replay`, `invariant`, `alloc_check`,
      `bench` (NFR-32).
      *Verify:* `cmake -B build -G Ninja` configures clean on a machine with no deps installed.
- [ ] **T1.2** `.clang-format` and `.clang-tidy` (the `PostToolUse` hook already expects them).
      **Partially done:** `.clang-format` exists; `.clang-tidy` is still missing.
- [x] **T1.3** `platform/platform.hpp` — `cpu_pause()`, `pin_thread_to_cpu()`, `prefault_pages()`.
      macOS-arm64: `__asm__ volatile("yield")`; pinning is a **no-op that returns false**, so callers
      can report honestly that they did not get a pinned core. Linux-x86: `_mm_pause`,
      `sched_setaffinity`, `mlockall`.
      *Verify:* compiles on macOS; the Linux branch is `#ifdef`-guarded and unreachable here.

## Phase 2 — Data structures

- [x] **T2.1** `common/types.hpp` — `Price`/`Quantity`/`OrderId`/`Seq` aliases, `Side`, and the
      sentinels (`BID_EMPTY = INT64_MIN`, `ASK_EMPTY = INT64_MAX`), plus `PRICE_SCALE = 10000`.
- [x] **T2.2** `common/object_pool.hpp` — pre-allocated `ObjectPool<T>` with a free list.
      *Not wired into the hot path yet* (Spec 004), but written now so Spec 004 is a swap, not a
      rewrite. *Test:* `unit/object_pool_test.cpp` — acquire/release, exhaustion returns null
      (**never** falls back to allocating).
- [x] **T2.3** `engine/order.hpp`, `engine/trade.hpp` — flat structs, intrusive `prev`/`next`, `int64`
      prices. *Test:* static_asserts on size and trivial-copyability.
- [x] **T2.4** `engine/price_level.{hpp,cpp}` — intrusive FIFO. `enqueue` (tail), `unlink(Order*)`,
      `head()`, `empty()`, `totalQty`.
      *Test:* `unit/price_level_test.cpp` — **FIFO order is preserved**; unlink from head / middle /
      tail; unlink the only element; `totalQty` stays correct across all of them.
- [x] **T2.5** `book/level_map.{hpp,cpp}` — open-addressing `price -> PriceLevel*`, power-of-two,
      linear probing, pre-sized. Tracks `bestPrice` as a field.
      *Test:* `unit/level_map_test.cpp` — **best price is correct after every insert and remove**;
      empty side returns the right sentinel; removing the best level **recomputes** the next best;
      probe collisions resolve correctly.
- [x] **T2.6** `book/order_id_map.{hpp,cpp}` — open-addressing `id -> Order*`.
      *Test:* `unit/order_id_map_test.cpp` — insert/find/erase, collisions, tombstones.

## Phase 3 — Matching (the actual engine)

- [x] **T3.1** `engine/order_book.{hpp,cpp}` — `processNewOrder`, per the loop in `plan.md`.
      **Branch on `order.side`.** Do not copy `planning/03-system-design.md`'s pseudocode; it rests
      into the bid book regardless of side.
- [x] **T3.2** *Test:* `unit/order_book_test.cpp` —
      - a buy at 101 into a resting sell at 100 trades at **100** (price improvement to the aggressor)
      - a non-crossing order **rests** and produces no trade
      - an order into an **empty book** rests, produces no trade, and does not touch a sentinel price
      - two orders at the same price fill **in arrival order** (FIFO)
      - a partially-filled resting order **keeps its queue position**
      - an aggressor sweeping several levels produces the trades **in price order**
      - the residual of a partly-filled aggressor rests at **its own limit price**, on **its own side**
- [x] **T3.3** *Test:* `tests/replay/` — the golden harness. Read a scenario file of commands, run
      them, serialize the trades, compare **byte-for-byte** against the committed reference (FR-47).
      Scenarios: `limit_match`, `partial_fill`, `full_fill`, `crossing_book`, `empty_book`,
      `fifo_same_price`, `price_improvement`.
      **The reference files must be generated and then read by a human before being committed.** A
      golden file blessed without being read is a golden file that enshrines a bug.

## Phase 4 — Measure (Principle 6)

- [x] **T4.1** `benchmark/velox_bench.cpp` — Google Benchmark + HdrHistogram_c. Build a populated book,
      warm up to steady state, time `processNewOrder`, report **p50/p99/p999** (never an average).
- [x] **T4.2** `benchmark/velox_alloc_check.cpp` — counting `operator new`/`delete`; warm up, reset,
      drive N orders, report bytes/op and allocs/op.
      **Expected to be non-zero at this spec.** That is the honest "before" number that Spec 004 will
      have to beat. Record it; do not hide it.
- [x] **T4.3** `benchmarks/baselines/hardware.md` — the real machine: CPU, cores, RAM, OS, compiler,
      flags. State plainly: **macOS-arm64, no core isolation available** (no `taskset`, no `numactl`,
      no `SCHED_FIFO`).
- [x] **T4.4** Run everything. **Record the real numbers** in `progress_report.md`, whatever they are.
      If p99 blows the 20 µs budget on the first try, the entry says so and says why. A bad first
      number is data; a missing one is a hole in the story.

## Phase 5 — Close out

- [x] **T5.1** Run the `latency-reviewer` sub-agent over `engine/` and `book/` (mandatory, NFR-36).
- [x] **T5.2** Run the `correctness-verifier` sub-agent.
- [x] **T5.3** Append the `progress_report.md` entry — what, why, how, and **every issue hit**.
- [x] **T5.4** Commit. **No `Co-Authored-By` trailer** (MANDATORY RULE 1).
- [x] **T5.5** `/perf-baseline` — promote the first real numbers to `benchmarks/baselines/summary.json`,
      with `durable: false` set honestly.
