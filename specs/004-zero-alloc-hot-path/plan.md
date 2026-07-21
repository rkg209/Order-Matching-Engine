# Plan — Spec 004: Zero-allocation, single-writer hot path

## Context

`specs/004-zero-alloc-hot-path/spec.md` is the headline latency spec: drive the matching hot path
to 0 bytes/op, prove it structurally rather than by discipline, and **measurably improve p50/p99/p999
without changing a single observable byte** of output.

The honest starting position, established by reading the code:

- **Most of the "work" list in the spec is already done**, because Spec 001 built the structures
  correctly the first time. `ObjectPool<Order>` (`common/object_pool.hpp`) is pre-allocated with a
  free list threaded through contiguous storage and returns `nullptr` on exhaustion (already surfaced
  as `SubmitStatus::RejectedPoolExhausted`, NFR-10 satisfied). `book::LevelMap` is a contiguous
  direct-indexed `PriceLevel` arena allocated once. `book::OrderIdMap` is open-addressed with
  backward-shift deletion. `PriceLevel` uses intrusive links, so there is no node type to allocate.
  `benchmarks/baselines/summary.json` already records `bytes_per_op: 0`.
- So Spec 004 is **not** "add pools". It is: close the remaining holes in the *proof*, fix the one
  genuine tail-latency defect, and report the before/after honestly — including if it is small.

**The one genuine latency defect.** `LevelMap::nextOccupied()` (`book/level_map.hpp:96`) is a linear
scan over the price array, called from `onLevelEmptied()` every time the best level empties, and from
`availableAgainst()` on every FOK pre-scan step. With the benchmark's config (min 1.00, max 200.00,
tick 0.01) that array is **19,901 slots**. On a book with depth at every price the next occupied
level is adjacent and the scan is ~1 read — which is exactly why the current benchmark, which rests
and crosses at a single price with 50 levels of nearby depth, *cannot see this at all*. On a thin or
gapped book the scan walks thousands of cache lines inside a single `submit()`. That is a p999 cliff
sitting in the middle of the hot path, and it is the change most likely to produce a real measured win.

**The proof holes.** `benchmark/velox_alloc_check.cpp` overrides `operator new`/`new[]` but **not the
aligned overloads** (`operator new(size_t, align_val_t)`), so an over-aligned allocation would be
silently uncounted. Its workload also only exercises `submit()` on limit orders — cancel, replace,
MARKET/IOC/FOK never run, so the Spec 002 paths are unproven at 0 bytes/op. And nothing mechanically
prevents someone adding `std::vector`, `throw`, `virtual`, or `printf` to `engine/` tomorrow.

**Intended outcome:** every Phase-A golden file byte-identical, `/alloc-check` proving 0 across *all*
order-lifecycle paths, a structural gate that makes regressions impossible rather than unlikely, and a
measured tail improvement on a benchmark scenario that can actually resolve it.

---

## Constraints

- Golden replay must stay **byte-identical**. If a golden file "needs regenerating", stop — that is a bug.
- p99 must not regress >20% vs `benchmarks/baselines/summary.json` (p50 11ns / p99 14ns / p999 18ns).
- `benchmarks/baselines/summary.json` is modified **only** by the deliberate `/perf-baseline` command.
- One change at a time, measured. No big-bang rewrite (spec: "How to work this spec").
- No `Co-Authored-By:` trailer, ever. Append a `progress_report.md` entry (next number: **[009]**).

## Decisions taken (confirmed with the user)

- **`Order`: reorder only, keep 80 bytes.** Do not narrow `ParticipantId`/`Seq` — that would change
  widths the journal (006) and gateway (007) inherit.
- **NFR-16: ship the utility, not the counters.** Add `common/cache.hpp` for Spec 005 to use. Add
  **no** hot-path telemetry counters now — an unread counter is pure added cost against an 11 ns p50.

---

## Work, in strict order (measure → change one thing → measure)

### T0 — Make the defect measurable (no engine change)

Without this step the main optimization is unmeasurable and we would be optimizing blind.

- `benchmark/velox_bench.cpp`: add **`BM_SweepThinBook`** — a book with liquidity at only a handful of
  widely separated prices across the configured range, where each aggressor consumes the best level
  entirely and forces `onLevelEmptied()` → `nextOccupied()` to walk. Follow the existing steady-state
  discipline in this file: alternate replenish/consume so the pool never exhausts, and keep the
  `RejectedPoolExhausted` guard (`velox_bench.cpp:118`) — a benchmark that degrades into measuring the
  reject path is worse than none.
- Add a second HdrHistogram pass over the same thin-book workload, reported with the same batching
  method as `reportLatencyDistribution()` (`velox_bench.cpp:195`), since per-call timing is below the
  ~41 ns clock tick on macOS-arm64.
- Run it. **Record the numbers in the scratchpad** — this is the "before" half of the deliverable.

### T1 — Close the proof holes (expected: no perf change)

1. `benchmark/velox_alloc_check.cpp`:
   - Override the **aligned** forms too: `operator new(size_t, std::align_val_t)`,
     `operator new[](size_t, std::align_val_t)` and their matching deletes. Currently an over-aligned
     allocation escapes the counter entirely.
   - Widen the measured loop past limit-submit-only: interleave `cancel()`, `replace()`, and
     MARKET / IOC / FOK submits so every Spec 002 path is covered at steady state. Keep the
     warmup → zero-counters → measure structure exactly as-is; the reset is the whole trick.
2. New `tests/structural/no_exceptions_tu.cpp`: a translation unit that includes every `engine/` and
   `book/` header, instantiates an `OrderBook` and drives submit/cancel/replace, compiled with
   **`-fno-exceptions -fno-rtti`** (`target_compile_options`, PRIVATE). If any hot-path header can
   throw or needs RTTI, this TU fails to compile. Mechanical proof, not a code review.
3. New `tests/structural/hot_path_grep_test.cpp` (or a CMake `add_test` running a small script):
   fail if `engine/**` or `book/**` contain `new `, `malloc`, `std::mutex`, `std::lock`, `virtual`,
   `throw`, `printf`, `iostream`, `spdlog`, `std::vector`, `std::map`, `std::unordered_map`,
   `std::function`, `shared_ptr`. Allow-list the two legitimate startup `std::make_unique` sites
   (`level_map.hpp:40`, `order_id_map.hpp:29-31`, `object_pool.hpp:28`) by exact line content, so the
   allow-list itself breaks if those lines move into a hot function.
4. `tests/CMakeLists.txt`: register both under label `unit`; keep the `alloc_check` label as-is
   (NFR-32 fixes the label set).

### T2 — `Order` cache layout

`engine/order.hpp`: reorder to the layout the user approved — inner-loop-hot fields first
(`remaining`, `participant`, `price`, `id`, `next`, `level`, `side` = 57 bytes, one line), cold fields
after (`quantity` as-submitted, `seq`, `prev`). Tighten `static_assert(sizeof(Order) <= 96)` →
`<= 80`, and keep the `is_trivially_copyable_v` assert. Add a comment stating *why* each field is on
the line it is on, citing the match loop in `engine/order_book.cpp:56-118` and `unlink()`
(`engine/price_level.hpp:41`) as the readers.

Pure reordering — all field access is by name, so no call site changes. Measure; expect small.

### T3 — Hierarchical occupancy bitset in `LevelMap` (**the main change**)

Replace the linear `nextOccupied()` scan with a two-level bitset summary, so best-price recovery is a
couple of `ctz`/`clz` instructions instead of a walk of up to ~20k slots.

In `book/level_map.hpp`:
- `l0_`: one bit per price slot, `ceil(numSlots/64)` words. Bit set ⇔ that level is non-empty.
- `l1_`: one bit per `l0_` word (312 words → 5 words for the benchmark config). A third level is not
  needed at this size; assert that `l1_` fits in a small fixed array and static-check the bound.
- Both allocated once in the constructor alongside `levels_` — pre-sized, never grows (NFR-11).
- `addOrder()` sets the bit when a level transitions empty→non-empty; the empty→non-empty and
  non-empty→empty transitions are the *only* mutation points, and they are already funnelled through
  `addOrder()` / `onLevelEmptied()` plus the `unlink()` call sites in `order_book.cpp`. **Audit those
  call sites carefully** — `cancel()` (`order_book.cpp:276-283`) and the STP path
  (`order_book.cpp:66-72`) both empty a level, and both must clear the bit.
- `nextOccupied(from)` becomes: mask off bits at/beyond `from` in the home word, `ctz`/`clz`; if the
  word is empty, consult `l1_` to jump to the next non-empty word. Direction depends on `side_`
  exactly as today.
- Keep `bool empty()` on `PriceLevel` as the source of truth and add a **debug-only** cross-check
  (`#ifndef NDEBUG`) asserting bit ⇔ `!levels_[i].empty()`. Zero cost in Release.

**Invariant to add** (Spec 003 harness, `tests/invariant/invariants.hpp`): after every operation, the
occupancy bitset agrees with `PriceLevel::empty()` for every slot, and `best()` equals the best
occupied slot found by brute-force scan. This is the property that catches a missed bit-clear — the
exact class of bug that would otherwise show up as a wrong best price weeks later.

Also add a `level_map_test.cpp` case for the gapped-book walk in both directions and at both range ends.

Run `/replay` immediately. Any divergence means a missed transition point.

### T4 — Cache utility + startup hardening

- New `common/cache.hpp`: `kCacheLineSize = 64`, `template<class T> struct alignas(64) CachePadded`,
  with a `static_assert(sizeof(CachePadded<T>) % 64 == 0)`. Unused by the engine today; Spec 005's
  SPSC ring head/tail indices are its first consumer. Small unit test asserting the alignment.
  **No hot-path counters** (per the decision above).
- `engine/order_book.cpp` constructor: `idMap_(cfg.maxOrders * 2)` assumes a power of two, because
  `OrderIdMap` uses `mask_ = capacity - 1`. Nothing validates that today — a `BookConfig.maxOrders`
  of, say, 1000 silently produces a broken mask. Round up to the next power of two at construction
  and document it. Startup-only, off the hot path.
- Optional: call `platform::prefaultPages()` (`platform/platform.hpp`) once at `OrderBook`
  construction; it honestly returns `false` on macOS, so it is a no-op here and real on Linux.

### T5 — Report

- Re-run everything; produce the after-numbers for both the existing steady scenario and the new
  thin-book scenario.
- `progress_report.md` entry **[009]**, strict format. Record real before/after for p50/p99/p999 on
  both scenarios. **If the improvement on the existing scenario is negligible, say so plainly** — the
  spec explicitly demands that rather than dressing it up. The expected honest story is: no change on
  the dense-book steady scenario (it never scanned), large p999 improvement on the thin-book scenario.
- Only if the numbers genuinely improved and are trusted, run `/perf-baseline` **deliberately** to
  promote them, and extend `summary.json` with the new `thin_book_sweep` scenario alongside
  `steady_limit_orders`. Never as a side effect.

---

## Files touched

| File | Change |
|---|---|
| `book/level_map.hpp` | **Main change.** Hierarchical occupancy bitset; O(1)-ish `nextOccupied()` |
| `engine/order.hpp` | Field reorder for cache line; tighten `static_assert` to 80 |
| `engine/order_book.cpp` | Audit every level empty/fill transition for bit maintenance; pow-2 `idMap_` sizing |
| `common/cache.hpp` | **New.** `kCacheLineSize`, `CachePadded<T>` (for Spec 005) |
| `benchmark/velox_bench.cpp` | **New** `BM_SweepThinBook` + thin-book Hdr pass |
| `benchmark/velox_alloc_check.cpp` | Aligned `new` overloads; widen workload to cancel/replace/MARKET/IOC/FOK |
| `tests/structural/no_exceptions_tu.cpp` | **New.** `-fno-exceptions -fno-rtti` compile gate |
| `tests/structural/hot_path_grep_test.cpp` | **New.** Forbidden-construct gate over `engine/`, `book/` |
| `tests/invariant/invariants.hpp` | **New invariant:** bitset ⇔ level emptiness, and `best()` correctness |
| `tests/unit/level_map_test.cpp` | Gapped-book `nextOccupied()` cases, both sides, both range ends |
| `tests/CMakeLists.txt` | Register the two structural targets under label `unit` |
| `progress_report.md` | Entry **[009]** |
| `benchmarks/baselines/summary.json` | Only via `/perf-baseline`, only if genuinely improved |

## Reuse — do not rewrite these

- `ObjectPool<T>` (`common/object_pool.hpp`) — already correct: contiguous, page-touching ctor,
  `nullptr` on exhaustion, no fallback allocation. Do not touch.
- `OrderIdMap` backward-shift `erase()` (`book/order_id_map.hpp:91`) — the tombstone hang documented
  in `progress_report.md [005]`. Do not "simplify" it.
- `PriceLevel::unlink()` / `reduceQuantity()` (`engine/price_level.hpp`) — the quantity-conservation
  ordering in `order_book.cpp:98-117` is load-bearing and was a real bug once.
- The `emptySentinel` / `kBidEmpty` / `kAskEmpty` scheme (`common/types.hpp:35`) — the bitset must
  keep returning these sentinels from `nextOccupied()` when nothing is found.
- The benchmark's steady-state alternation and pool-exhaustion guard — copy the pattern for the new
  scenario rather than inventing one.

## Verification (after **every** T-step, not just at the end)

```bash
cmake --build build
ctest --test-dir build -L replay      # MUST be byte-identical. Divergence = stop, you have a bug.
ctest --test-dir build -L unit
ctest --test-dir build -L invariant   # includes the new bitset-consistency invariant
ctest --test-dir build -L alloc_check # 0 allocs/op, 0 bytes/op
./build/benchmark/velox_bench         # compare vs p50 11 / p99 14 / p999 18 ns
```

Plus, per CLAUDE.md, after any `engine/` or `book/` edit:

- the **`latency-reviewer`** sub-agent must report zero violations (spec DoD);
- the **`correctness-verifier`** sub-agent for the replay/invariant sweep;
- `/bench` via the **`benchmark-runner`** sub-agent so the raw benchmark output stays out of context.

**Done** when: all five labels green, every golden file untouched in `git status`, `/alloc-check`
reports 0 across all lifecycle paths, `latency-reviewer` is clean, before/after numbers for both
scenarios are recorded, and `progress_report.md [009]` exists.
