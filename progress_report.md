# Velox — Progress Report

> The running story of how this project was built: **what** we did, **why** we did it, **how** we did
> it, and what broke along the way.
>
> This file is **append-only**. Entries are numbered sequentially and never reused, never edited, never
> deleted. If an entry turns out to be wrong, a later entry corrects it and says so explicitly.
>
> Read top-to-bottom, this file should tell a stranger the entire development history of the engine —
> including the dead ends. Maintained per **MANDATORY RULE 2** in `CLAUDE.md`.

---

## [001] 2026-07-14 — Author the project definition and planning documents

**What:** Wrote `order-matching-engine-spec.md` (the authoritative 463-line project definition) and
five supporting documents in `planning/`: `01-project-summary.md`, `01-requirements.md`,
`02-architecture.md`, `03-system-design.md`, `04-database-design.md`, `04-database-schema.sql`,
`05-api-design.md`, `05-openapi.yaml`.

Together these define Velox: a low-latency in-memory order-matching engine and mini-exchange in
C++20, with a single-threaded deterministic matching hot path, lock-free SPSC ring ingress, a
journaled event-sourced recovery story, a binary order gateway, a market-data feed, and a live
visualizer. They also fix the 52 functional requirements (FR-1…FR-52), 37 non-functional
requirements (NFR-1…NFR-37), 12 constraints (CON-1…CON-12), and the ordered spec backlog 001–011.

**Why:** This is a portfolio project whose entire pitch is *measured* tail latency and *proven*
correctness. That pitch only survives interview scrutiny if the requirements — especially the
performance budget and the honesty rules around measurement — are written down *before* any code
exists, so that they constrain the code rather than being reverse-engineered to flatter it.

**How:** Top-down. The root spec locks five decisions that are explicitly not re-litigated later
(C++20 single-phase, full mini-exchange scope, web visualizer, single-threaded LMAX-style hot path,
price-levels + intrusive FIFO + id-map order book) and then derives everything else from them. The
planning documents expand each area into concrete requirements with numeric budgets.

**Issues:** None at the time — the defects in these documents were only discovered later, in [003].

---

## [002] 2026-07-14 — Install toolchain and initialize the repository

**What:** Installed the missing build toolchain via Homebrew — `cmake` 4.4.0, `ninja` 1.13.2, and
`llvm` 22.1.8 (which supplies `clang-format` and `clang-tidy`). Ran `git init -b main`, wrote
`.gitignore`, and made the repository real. Before this, the project directory contained prose and
nothing else: no code, no build system, and no git repository at all.

**Why:** Nothing in the plan — not the hooks that auto-format on write, not the CI gates, not the
benchmark baselines — can exist without a build system and version control underneath them.

**How:** Homebrew, because the project targets macOS-arm64 for development. Two consequences worth
recording:

- **`llvm` is keg-only** on Homebrew: macOS ships its own toolchain, so Homebrew refuses to symlink
  its LLVM into `/opt/homebrew/bin`. `clang-format` and `clang-tidy` therefore live at
  `/opt/homebrew/opt/llvm/bin/` and are **not on `PATH` by default**. Every hook script that invokes
  them must probe that path explicitly rather than assuming a bare `clang-format` resolves.
- **Third-party dependencies (GoogleTest, Google Benchmark, HdrHistogram_c) are fetched by CMake
  `FetchContent`**, not vcpkg and not Conan. The planning documents never decided this — they list
  the libraries but name no acquisition mechanism. `FetchContent` was chosen so that `cmake -B build`
  works on a clean machine with zero pre-installed dependencies, which keeps the "one command to
  build" promise honest for anyone cloning the repo.

**Issues:** The system had **no CMake at all** (`cmake: NOT FOUND`), despite the entire architecture
being specified around it. Resolved by installing it. Worth noting because it means none of the
planning documents had ever been executed against a real build.

---

## [003] 2026-07-14 — Establish the rules layer, and resolve four contradictions in the planning docs

**What:** Wrote the three documents that govern everything downstream:

- **`CLAUDE.md`** — the operating rules Claude Code reads every session: the five hot-path
  non-negotiables, the performance budget, build/test/bench commands, and two mandatory rules (the
  git-trailer ban and the requirement to maintain this very file).
- **`.specify/memory/constitution.md`** — the six governing principles (performance budget is a hard
  gate · correctness is proven not claimed · determinism is mandatory · single-writer zero-allocation
  hot path · thin vertical slice first · measure from day one), with the hard numbers attached:
  p50 ≤ 2 µs, p99 ≤ 20 µs, p999 ≤ 100 µs, ≥ 1M orders/sec, 0 bytes/op, and a > 20% p99 regression as
  a build failure.
- **`progress_report.md`** — this file.

**Why:** Spec-driven development only works if the principles are written before the specs, and the
specs before the code. Without a constitution, every downstream spec silently renegotiates the
performance budget. Two rules in particular were requested and are now enforced at the top level:
never write a `Co-Authored-By` trailer into a commit message (it breaks GitHub pushes), and append an
entry here after every meaningful change.

**How:** The constitution takes its six principles from Section 10 of the root spec and attaches the
numeric budgets from `01-requirements.md`, which the root spec states only qualitatively. Beyond
transcription, it adds two **carve-outs** that the requirements imply but never state — without them
the tooling would fight itself:

1. **`steady_clock` is permitted on the hot path**, and only for latency capture at cycle boundaries.
   NFR-18 bans clock reads, but NFR-4 demands HdrHistogram latency measurement *of the hot path*.
   Stated explicitly so the `latency-reviewer` agent does not flag its own measurement apparatus.
2. **The throughput headline is a no-durability number.** FR-18 requires `fsync` before a command is
   acked; NFR-7 requires 1M orders/sec. These are physically irreconcilable — `fsync` costs tens to
   hundreds of microseconds, so a million of them per second is impossible on any storage device.
   They only coexist because the `bench` startup mode skips the journal entirely. The constitution
   now says so out loud, and requires the number always be reported that way. This is exactly the
   kind of thing an interviewer probes, and the honest answer is a strength.

**Issues:** Reading the planning documents end-to-end surfaced four real defects. All are now
recorded in `CLAUDE.md` under "Known planning-doc defects" so they cannot quietly propagate into code:

1. **The database contradiction (the big one).** `01-requirements.md` **CON-8** states plainly that
   there is *no external database or persistent store* and that the journal is the sole durability
   mechanism. But `04-database-design.md` and `04-database-schema.sql` specify **PostgreSQL 16**, and
   `05-api-design.md`/`05-openapi.yaml` specify a **JWT-authenticated REST admin API**. These cannot
   both be true. **Resolved: CON-8 wins.** The core stays in-memory and journal-only, which is what
   the entire low-latency pitch rests on. The Postgres and REST work is preserved as **Specs 012 and
   013**, marked optional, placed at the back of the backlog, and blocking nothing. Worse, the `.md`
   and the `.sql` define *two different, mutually incompatible schemas* (singular vs plural table
   names; one `velox` schema vs three) — so even if we wanted that work, it would need redesign, not
   transcription. Both facts are recorded in the deferred specs.
2. **All five planning files are truncated mid-sentence.** Generation was cut off.
   `02-architecture.md` loses its Security Architecture and Technology Stack sections;
   `03-system-design.md` loses Event Flows, State Transitions, and Design Patterns;
   `04-database-design.md` loses **the snapshot binary format** (which we actually need for Spec 006);
   and `05-openapi.yaml` loses its **entire `components:` block** while containing ~50 `$ref`s into it
   — meaning the OpenAPI document does not parse. (Both `04-database-schema.sql` and
   `05-openapi.yaml` also begin with a literal markdown code fence, so neither is machine-loadable
   as-is.) These gaps get filled by the specs, not by trusting the docs.
3. **The matching-loop pseudocode in `03-system-design.md` has a side bug.** It always rests residual
   orders into the *bid* book (`bidLevels.getOrCreate(...)`) and always removes filled levels from the
   *ask* book, with no branch on `order.side` — so a resting SELL would land in the bids. It also
   conflates `orderType` (LIMIT/MARKET) with `timeInForce` (DAY/GTC/IOC/FOK), branching on
   `timeInForce == LIMIT`. The pseudocode must not be copied verbatim; Spec 001 writes the loop from
   the semantics, not from the doc.
4. **The outbound ring is declared SPSC but given two consumers.** `03-system-design.md` §1.6 types it
   as `SpscRing<OutboundEvent>` and then hands it to both the `ExecutionReportRouter` and the
   `MarketDataPublisher`. A genuine single-producer-single-consumer ring cannot do that. It needs
   either two rings or a Disruptor-style multi-consumer barrier with independent cursors. Flagged for
   resolution in Spec 005/008 rather than papered over.

---

## [004] 2026-07-14 — Build the Claude Code tooling layer

**What:** Wrote `.claude/` — the guardrails that make the non-negotiables mechanical rather than
aspirational:

- **9 slash commands** (`.claude/commands/`): `/bench`, `/replay`, `/invariants`, `/alloc-check`,
  `/recover-test`, `/profile`, `/perf-baseline`, `/spec`, `/progress`.
- **4 skills** (`.claude/skills/`): `low-latency-cpp`, `order-book-internals`, `matching-semantics`,
  `benchmark-methodology` — model-invoked, so the domain rules load automatically when the task
  matches, without costing context on every turn.
- **4 sub-agents** (`.claude/agents/`): `latency-reviewer` (MUST be used after any `engine/`/`book/`
  edit), `correctness-verifier`, `benchmark-runner`, `spec-author` — each in its own context window so
  heavy verification output does not pollute the main session.
- **Hooks** (`.claude/settings.json` + `.claude/scripts/`): auto-format and hot-path-lint on every
  write; a Bash guard; session-start context injection.

**Why:** NFR-35 and NFR-36 require that hot-path modules be formatted and scanned after *every* edit.
A prompt instruction is advisory and degrades over a long session; a hook is deterministic and runs
identically every time. The whole point of encoding the latency invariants in tooling is that they
survive the moment you forget them.

**How:** Three scripts do the work. `format-and-lint.sh` runs `clang-format -i` on the written file
and then `hot-path-lint.sh`, which greps `engine/`/`book/` for the forbidden constructs (`new`,
`malloc`, `throw`, `virtual`, `std::mutex`, `std::lock_guard`, `std::cout`/`printf`/spdlog,
`push_back`, `system_clock`, `rand`) and reports violations back to Claude as feedback.
`inject-context.sh` injects the current p99 baseline and the non-negotiables at session start.

`guard-bash.sh` blocks destructive commands — and, specifically, **blocks any `git commit` whose
message carries a `Co-Authored-By` trailer**. The user's rule is thereby enforced by the harness and
not merely by instruction: the failure mode of "Claude forgets" is designed out rather than trusted
away.

Both linters deliberately **skip `steady_clock`** per the constitution's carve-out ([003]), so the
latency-measurement apparatus is not flagged as a violation of the rules it exists to verify.

**Issues:** The `clang-format` binary is not on `PATH` (keg-only Homebrew LLVM, see [002]), so the
scripts probe `/opt/homebrew/opt/llvm/bin/clang-format` first and fall back to a bare `clang-format`,
degrading gracefully to a no-op if neither is found. A missing formatter must never block an edit.

Separately, testing the Bash guard revealed that it **failed open**: it extracted the command with
`jq`, and when `jq` could not parse the payload, the variable came back empty and the script exited 0
— *allowing* the command. A guard that permits everything the moment its input is malformed is not a
guard, and malformed input is precisely what an evasion would look like. Fixed to **fail closed**: on
a parse failure it now scans the raw payload text instead of waving the command through. Over-blocking
is cheap here; under-blocking is not. Verified with six cases, including a deliberately malformed
payload carrying a `Co-Authored-By` trailer — which is now correctly blocked.

---

## [005] 2026-07-14 — Implement Spec 001: the core order book — and find four real bugs doing it

**What:** The thin vertical slice is complete and green. A single-instrument limit order book that
matches by price-time priority, emits trades, and rests residuals:

- `common/` — `types.hpp` (scaled `int64` prices, `Side`, the empty-book sentinels), `object_pool.hpp`
- `platform/` — `platform.hpp`, the macOS/Linux shim (`cpuPause`, `pinThreadToCpu`, `prefaultPages`)
- `engine/` — `order.hpp`, `trade.hpp`, `price_level.hpp` (intrusive FIFO), `order_book.{hpp,cpp}`
- `book/` — `level_map.hpp` (direct-indexed price levels + best-price tracking), `order_id_map.hpp`
- `tests/` — 44 unit tests, 7 golden replay scenarios (byte-for-byte), 1 determinism test
- `benchmark/` — `velox_bench` (Google Benchmark + HdrHistogram), `velox_alloc_check`

**Everything passes: 54/54 tests across all five CTest labels.**

Measured on Apple M4, macOS 26.5, `-O3 -DNDEBUG`, **no core isolation** (see `hardware.md`):

| Metric | Measured | Budget |
|---|---|---|
| p50 (per order, batched) | **11 ns** | 2,000 ns |
| p99 | **14 ns** | 20,000 ns |
| p999 | **18 ns** | 100,000 ns |
| max | 279 ns | — |
| Throughput | **85.6 M/s** (resting) · 221 M/s (crossing) | ≥1 M/s |
| **Hot-path allocation** | **0 bytes/op, 0 allocs/op** | 0 |

These are far inside budget — but the budgets were written for the **end-to-end** path (gateway → ring
→ engine → exec report), and this measures **only the matching call in isolation**. The isolated call
being ~100× under an end-to-end budget is expected, not a triumph. The real test comes in Spec 009.

**Why:** Spec 001's DoD requires a byte-identical golden replay and a *real* latency number from day
one (constitution Principle 6) — you never optimize blind, and you never discover in month three that
the number was always bad.

**How:** Two decisions worth recording.

*Direct-indexed price levels instead of a hash map.* The root spec offers both ("array-indexed when
ticks are bounded, else a tree map"). I took the array. The reason is the one operation that is not
O(1): when the best level empties, the next-best price must be found. A hash map cannot do this
without scanning every bucket — an O(capacity) hit on a *frequent* operation, landing squarely in the
p99. A contiguous array lets us walk outward from the emptied price, which in a book with any depth
finds the next level in a handful of cache-hot reads. The cost is that the price range must be bounded
and memory scales with the range rather than with occupancy. For one instrument that is a few MB, and
it buys worst-case O(1).

*Time priority is a counter, not a clock.* `Order::seq` is a monotonic integer. A timestamp would make
replay depend on wall-clock — the same journal replayed tomorrow would produce different priority and
therefore different trades, and byte-identical replay would be impossible. Arrival *order* carries
identical semantics and is deterministic by construction. This is Principle 3 showing up as a
data-structure decision rather than a rule to remember.

**Issues:** Four real bugs. All four were caught by tooling rather than by inspection, which is the
entire argument for building the tooling first.

**(1) Quantity conservation broke silently — caught by unit tests.**
The fill loop did `resting->remaining -= qty` *before* calling `level->unlink(resting)`. But `unlink()`
decrements the level's aggregate quantity by `o->remaining` — which is now **0** for a fully-filled
order. So a full fill subtracted *nothing* from the level's total, and the level went on advertising
liquidity that had already traded away. Two tests caught it immediately
(`FifoWithinAPriceLevel`, `ResidualSellRestsInTheAsksNotTheBids`).
Fixed by calling `reduceQuantity(qty)` unconditionally, *before* any unlink. Invariant 1 (quantity
conservation) is not decoration — it is the thing that was broken.

**(2) The benchmark was timing its own instrumentation — caught by cross-validation.**
The HdrHistogram loop nested two `steady_clock::now()` calls *inside* the batch window it was timing.
A clock read costs ~25 ns on this machine; the operation being measured costs ~11 ns. So two reads per
iteration turned a ~11 ns operation into a reported **58 ns** — a 5× overstatement that was almost
entirely my own measurement apparatus. It was Google Benchmark's independent number *disagreeing by
10×* that exposed it. **When two instruments disagree by an order of magnitude, one of them is lying;
find out which before publishing either.** Fixed by splitting into two passes, with nothing but
`submit()` inside the timed window.

**(3) The benchmark was measuring the REJECT path — caught by the same disagreement.**
Worse, and more embarrassing. `BM_SubmitRestingOrder` only ever *rested* orders and never matched
them. The order pool holds ~1M orders; Google Benchmark ran **441 million** iterations. So after the
first million the pool was exhausted and `submit()` was returning `RejectedPoolExhausted` immediately
— meaning **~99.8% of the "matching latency" being reported was the cost of the engine refusing to do
work.** It reported a beautiful 6 ns/op and it was nonsense.

This is exactly what the `benchmark-methodology` skill means by *"if a number looks too good, it
probably is."* Fixed by making the workload steady-state (alternate a resting bid with a sell that
crosses and consumes it, so the book never grows) and by adding a **pool-exhaustion guard that fails
the benchmark loudly** rather than letting it quietly report a fast number for a broken engine. The
honest figure is **11.7 ns/op — nearly 2× *slower* than the lie.**

**(4) `OrderIdMap` degraded to O(n) under sustained churn — caught by the benchmark HANGING.**
The map used **tombstones** on erase: a "deleted but still probe-transparent" third state, which is the
textbook-correct way to delete from an open-addressed table. It is also a slow-motion disaster.
Tombstones are never reclaimed, so in an engine where orders continually rest and fill, the table
saturates with them — and once it does, every `find()` probes the *entire* table, because a tombstone
cannot terminate a probe. The map silently degrades from O(1) to O(capacity).

The symptom was the benchmark **hanging** — not crashing, not failing an assertion. Hanging. Every
functional test stayed green throughout, because the bug is invisible until you have cycled far more
orders through the map than it can hold. **That is how this bug would have presented in production:
hours after deploy, under sustained load, with a clean test suite.**

Fixed with **backward-shift deletion**, which removes the entry and pulls back any element that would
become unreachable, leaving the table exactly as if the key had never been inserted. No tombstones, no
degradation, no rehash, still zero allocation. Added `SustainedInsertEraseChurnDoesNotDegrade`, which
churns 200× the table's capacity through it — the test that would have caught this on day one.

**A note on all four.** Bugs 2, 3, and 4 were in the *measurement and supporting infrastructure*, not
in the matching logic. That is worth sitting with: the engine was essentially right, and the things
telling me the engine was right were wrong. This is precisely why Principle 6 demands a latency harness
from day one — not because there was anything to optimize in Spec 001, but because a measurement
apparatus that has never been challenged is an apparatus you cannot trust when it finally matters.

---

## [006] 2026-07-14 — Fix the session-start hook reporting false spec status

**What:** `.claude/scripts/inject-context.sh` now reads each spec's **declared** `**Status:**` line
from its `spec.md`, instead of guessing from which files happen to exist. It also flags the two
deferred specs explicitly and notes whether `plan.md`/`tasks.md` are present.

**Why:** The hook was reporting **`001-core-order-book — IN PROGRESS`** and
**`000-constitution — backlog`** at session start, when both are complete. Its rule was "has
`tasks.md` ⇒ in progress", which is true exactly until a spec is *finished* — at which point the
artifact that proves the work was done becomes the evidence it is still ongoing. It also had no way to
represent COMPLETE or DEFERRED at all, so specs 012/013 looked like ordinary backlog items rather than
work that is explicitly optional and that nothing may depend on.

This is injected into the context of **every future session**. A status line that lies is worse than no
status line, because it is trusted: the next session would have started by re-opening finished work,
or by treating a deferred, CON-8-violating spec as a normal next step.

**How:** Parse the `**Status:**` line each spec already carries (`COMPLETE` / `IN PROGRESS` /
`BACKLOG` / `DEFERRED`) and report that, falling back to `backlog (no status line in spec.md)` when a
spec has none — so a missing status is visible rather than silently defaulting to something plausible.
The single source of truth for a spec's state is now the spec itself, which is where it belongs.

**Issues:** Caught only because a stale background-task notification happened to reprint the
session-start context, making the wrong output visible. Worth noting: the tooling built to enforce
correctness is not itself exempt from being wrong, and nothing was checking it.

---

## [007] 2026-07-21 — Implement Spec 002: the full order lifecycle

**What:** Widened `OrderBook` from LIMIT-only (Spec 001) to the full lifecycle: `OrderType`
(Market/Ioc/Fok, Limit default), `StpPolicy` (CancelAggressor/CancelPassive/CancelBoth,
CancelAggressor default), an enriched `OrderResult` return type, `cancel()`, `replace()`, and
self-trade prevention inside the match loop. `submit()`'s old body was refactored into a shared
private `matchInto()` (the match loop, now STP-aware and OrderType-aware via a `crosses()`
predicate that treats Market as "always crosses") plus per-type residual disposition in
`submitEx()`. FOK gets a dedicated non-mutating pre-scan, `availableAgainst()`, that walks the
opposite book without touching pool/id-map/levels — needed because matching is destructive and
FOK cannot roll back a partial match. `LevelMap` gained a const `nextOccupied()`, factored out of
`onLevelEmptied()`, so the pre-scan can walk price levels without a mutating method. Added 13 new
golden replay scenarios (`tests/replay/scenarios|golden/*`) covering every FR-48 case plus the
named edge cases (FOK one-short, cancel-after-fill, replace-into-cross, STP stopping mid-sweep,
market exhausting the book, cancel/replace resetting time priority), 19 new unit tests in
`tests/unit/order_book_test.cpp`, and extended the replay scenario grammar
(`NEW ... [IOC|FOK]`, `MARKET`, `CANCEL`, `REPLACE`) in `tests/replay/replay_test.cpp`.

**Why:** Spec 001 shipped LIMIT-only matching with `OrderIdMap`, `Order::level`, and
`Order::participant` deliberately built but unused, so this spec cashes them in. Satisfies
`specs/002-order-lifecycle/spec.md` FR-48 (all 11 lifecycle cases) and NFR-22 (no order lost or
double-filled), which is why quantity conservation is asserted mechanically
(`QuantityIsConservedAcrossMixedOrderTypes`) rather than eyeballed.

**How:** Followed `.claude/plans/002-order-lifecycle.md`'s commit sequencing: types first (no
behavior change), then the match-loop refactor (still LIMIT-only, proven neutral by the 7
untouched Spec 001 goldens), then MARKET/IOC, then the FOK pre-scan, then cancel/replace, then
STP, then the conservation test. STP's interaction with the FOK pre-scan is the one subtle piece
of design: under `CancelAggressor`, the scan must **stop** counting at a same-participant order,
not skip past it — skipping would report liquidity FOK could never legally reach, and it would
then fail mid-execution instead of rejecting cleanly up front. A dedicated unit test
(`FokPreScanStopsAtSelfTradeRatherThanSkippingPastIt`) plants reachable liquidity behind the
collision specifically to catch a "skip" implementation that would wrongly report it as fillable.
`replace()` deliberately re-validates and re-looks-up `oldId` inside `cancel()` rather than
sharing state between the two calls — accepted because Spec 002 is a correctness-breadth spec,
not a latency one, and `cancel`/`replace` are not p99-critical paths (the `latency-reviewer`
sub-agent confirmed this reasoning holds and found no hot-path violations elsewhere: no
allocation, no locks, no virtual dispatch, and `availableAgainst()` is genuinely non-mutating).

**Issues:** The refactor initially introduced two real bugs, both caught before commit rather than
by a later incident:

1. **Pool exhaustion silently swallowed.** The first version of `restResidual()` returned `void`
   and simply did nothing on `pool_.acquire() == nullptr`, so `submitEx()` unconditionally reported
   `Ok` and `rested = true` even when the residual never actually rested — a correctness
   regression from Spec 001's `submit()`, which correctly returned `RejectedPoolExhausted`. Fixed
   by making `restResidual()` return `bool` and having the caller translate `false` into
   `RejectedPoolExhausted`, restoring the original backpressure contract (NFR-10).

2. **STP's default policy broke the benchmark's steady-state assumption.** `benchmark/velox_bench.cpp`
   reused `participant = 1` for both the resting book and the alternating rest/cross workload — a
   Spec 001 convenience that meant nothing when self-trade prevention didn't exist. With
   `StpPolicy::CancelAggressor` now the default, every "crossing" order in the benchmark
   self-traded against the resting book instead of matching it, so resting orders piled up
   unboundedly instead of being consumed, and the HdrHistogram batch measurement reported p50=29ns
   / p99=39ns against a 14ns p99 baseline — a >100% regression that looked like a real hot-path
   problem. It was not: after giving the resting leg and the crossing leg of each benchmark
   workload distinct participant ids (and distinct from each other), the measured p50/p99 returned
   to 13ns/15-16ns, within the 20% gate against `benchmarks/baselines/summary.json`
   (p50=11ns/p99=14ns). The lesson matches [005]'s: a benchmark's assumptions can silently break
   when the semantics underneath it change, and the honest fix is to update the workload to match
   real trading (distinct counterparties), not to tune the engine against a broken measurement.

Verified: `latency-reviewer` found no hot-path violations; `correctness-verifier` confirmed 63/63
unit tests, 21/21 replay tests (7 untouched Spec 001 goldens + 13 new + the determinism check)
green, and `velox_alloc_check` at 0 bytes/op, 0 allocs/op.

## [008] 2026-07-21 — Implement Spec 003: the invariant/property-test harness — and find a real bug in `cancel()`

**What:** Added `tests/invariant/{invariants.hpp, schedule.hpp, shrink.hpp, property_test.cpp}` —
a randomized property-test harness that asserts eight invariants (I1 quantity conservation, I2
sequence monotonicity, I3 no crossed book, I4 FIFO fairness, I5 id-map/level mutual consistency,
I6 no empty level left occupied, I7 best-price-is-real, I8 level-aggregate/pool accounting)
after **every single operation** across ten adversarial schedule profiles (`Uniform`,
`HeavyCancel`, `SinglePrice`, `AlternatingCross`, `DrainRefill`, `LevelChurn`, `StpHeavy` x3 —
one per `StpPolicy` — `ReplaceHeavy`, `TinyPool`, `NarrowRange`), with a ddmin-style shrinker that
reduces any failure to a minimal counterexample rendered in the golden-replay scenario grammar.
Added three const, noexcept, non-virtual introspection accessors to enable it:
`OrderIdMap::forEach`, `LevelMap::levelAtIndex`, `OrderBook::sideView`/`::pool`. Wired as
`ctest -L invariant` (`velox_invariant_tests`) in `tests/CMakeLists.txt`. Also fixed a real bug in
`engine/order_book.cpp`'s `OrderBook::cancel()`.

**Why:** Satisfies `specs/003-invariants-property-tests/spec.md` FR-49 / NFR-21 / NFR-22 and
constitution Principle 2 ("correctness is proven, not claimed"). Golden replay (Spec 001/002)
only proves the engine is right on sequences a human thought to write; this spec generates the
sequences nobody would have written by hand.

**How:** Followed `.claude/plans/003-invariants-property-tests.md`'s order of work: accessors
first (latency-reviewed, `/replay` confirmed byte-identical, `/bench` no p99 regression), then the
checker against a hand-written 7-op sanity schedule, then a deliberate breakage (removed
`level->reduceQuantity(qty)` from `matchInto`) to confirm the checker names the right invariant
before trusting it against random input — it fired `I8.LevelAggregateAndPool` immediately and
shrunk to 2 ops, exactly as predicted. Only then the generator, the shrinker, and the CMake
wiring. The `Ledger` (the harness-side per-order accounting model) is driven by explicit
`onSubmit`/`onCancel`/`onReplaceOldWithdrawn`/`onTrades` calls rather than trying to infer intent
from `OrderResult` alone, because `replace()` discards `cancel()`'s own result internally
(`order_book.cpp:309`) — the old order's `remaining` has to be read by the harness *before*
calling `replace()`, the same pattern the plan called out up front.

**Issues:** The very first full run of all 13 property tests found two real bugs, not
hypothetical ones:

1. **A bug in the checker itself (test-only, no engine impact).** `Ledger::onSubmit`
   unconditionally reset an id's fill counters on every call, including when the submit was a
   pre-seq reject (`RejectedDuplicateId`, but also `RejectedInvalidQuantity` /
   `RejectedPriceOutOfRange` — `order_book.cpp` checks quantity and price range *before* the
   duplicate-id check, so either can fire while the id is still legitimately resting) against an
   id that was **already resting**. That reset the ledger's view of a live, untouched order,
   producing false `I1.QuantityConservation` failures across nearly every profile. Fixed by
   threading a `wasAlreadyResting` flag (read from `book.orders().find(id) != nullptr` before the
   call) through `onSubmit`, and skipping the reset when a pre-seq reject hits an id that was
   already resting.

2. **A real engine bug in `OrderBook::cancel()`.** After the ledger fix, nine of the eleven
   remaining profiles still failed — all on `I7.BestPriceIsReal`, all shrinking to the same
   2-3-op shape: two orders resting at the same price, then a cancel of one of them. `cancel()`
   called `sideOf(side).onLevelEmptied(price)` **unconditionally** after unlinking, but
   `LevelMap::onLevelEmptied()` only checks `if (p != best_) return;` — it does not itself check
   whether the level actually emptied. So cancelling one of two co-priced orders at the best price
   wrongly walked `nextOccupied()` *past* that price looking for the next occupied level, even
   though the other order was still resting there, corrupting the tracked best price. Fixed by
   capturing the level pointer before `unlink()` and guarding the call with
   `if (level->empty())` — the same pattern `matchInto()` already used correctly at both of its
   own `onLevelEmptied()` call sites. Confirmed via `latency-reviewer` (the added
   `PriceLevel::empty()` check is a single noexcept pointer compare, no allocation/exception/lock
   introduced), `ctest -L replay` (stayed byte-identical — no existing golden happened to
   exercise this exact pattern), and `/bench` (p99 stayed within the 20% gate against
   `benchmarks/baselines/summary.json`, three repeat runs in the 14-16ns band vs. a 14ns
   baseline, well under the 20µs budget).

Verified: all 98 tests green (63 unit + 21 replay + 14 invariant, including the hand-written
sanity test and the same-seed-same-trade-digest determinism test), `velox_alloc_check` at 0
bytes/op, `VELOX_SCHEDULES=2000` long soak green across all ten profiles.

## [009] 2026-07-21 — Implement Spec 004: close the zero-alloc proof holes, fix the real p999 defect

**What:** Per `.claude/plans/004-zero-alloc-hot-path.md`:

- **T0:** Added `BM_SweepThinBook` (`benchmark/velox_bench.cpp`) and a matching HdrHistogram pass
  (`reportThinBookLatencyDistribution()`) — a book with real depth at only ~20 widely-spaced
  ($5 apart) prices, so emptying the best level forces a genuine gap walk instead of the dense
  benchmarks' always-adjacent-level case.
- **T1:** `benchmark/velox_alloc_check.cpp` now overrides the *aligned* `operator new`/`delete`
  overloads too (an over-aligned allocation previously escaped the counter entirely), and its
  workload was widened from limit-submit-only to a 6-op cycle covering LIMIT rest, LIMIT cross,
  `cancel()`, `replace()`, MARKET, and IOC/FOK — every Spec 002 path, at steady state. Added
  `tests/structural/no_exceptions_tu.cpp` (compiles every `engine/`/`book/` header against
  `-fno-exceptions -fno-rtti`) and `tests/structural/hot_path_grep_test.py` (fails on `new `,
  `malloc`, `std::mutex`, `virtual`, `throw`, `std::vector`, etc. anywhere in `engine/`/`book/`,
  comment-stripped, with an exact-line-content allow-list for the legitimate startup
  `std::make_unique` sites). Both wired into `ctest -L unit`.
- **T2:** `engine/order.hpp`: reordered `Order`'s fields — hot (`remaining`, `participant`,
  `price`, `id`, `next`, `level`, `side`) first, cold (`quantity`, `seq`, `prev`) after — and
  tightened `static_assert(sizeof(Order) <= 96)` to `<= 80` (the actual, exact size; not loosened
  for headroom).
- **T3 (the main change):** Replaced `LevelMap::nextOccupied()`'s linear scan
  (`book/level_map.hpp`) with a two-level occupancy bitset (`l0_`: one bit per price slot; `l1_`:
  one bit per `l0_` word), both allocated once at construction alongside `levels_`.
  `addOrder()` sets a bit on the empty→non-empty transition; `onLevelEmptied()` now
  unconditionally clears the bit for its slot (it is only ever called immediately after a level
  became empty — verified all three call sites in `order_book.cpp`, the STP path, the main
  `matchInto()` drain loop, and `cancel()`, are already gated on `level->empty()`) and only
  recomputes `best_` when the emptied slot was the tracked best. `findSetBitBelow()`/
  `findSetBitAbove()` walk the hierarchy with `std::countl_zero`/`std::countr_zero`. Added a
  debug-only (`#ifndef NDEBUG`) cross-check against `PriceLevel::empty()` in
  `setOccupied()`/`clearOccupied()`, a new public `occupiedBit()` introspection accessor, a new
  **I9.OccupancyBitsetConsistency** invariant (`tests/invariant/invariants.hpp`) checked after
  every op across all twelve randomized schedule profiles, and new `level_map_test.cpp` cases for
  gapped walks in both directions and at both range ends.
- **T4:** New `common/cache.hpp` (`kCacheLineSize`, `CachePadded<T>`, unit-tested) for Spec 005's
  SPSC ring to consume — no hot-path counters added, per the decision recorded in the plan.
  `OrderBook`'s constructor now rounds `idMap_`'s capacity up to the next power of two
  (`std::bit_width`-based `nextPowerOfTwo()`) instead of assuming `maxOrders * 2` already is one —
  previously a non-power-of-two `maxOrders` would silently corrupt `OrderIdMap`'s mask-based
  probing. Also added a startup-only `platform::prefaultPages()` call (honest no-op on macOS,
  real on Linux).

**Why:** Satisfies `specs/004-zero-alloc-hot-path/spec.md`: prove 0 bytes/op structurally rather
than by discipline, close the two proof holes identified while reading the code (aligned-new
blind spot, limit-only workload), and fix the one real tail-latency defect found — a full
`~20k`-slot linear scan inside `onLevelEmptied()`/`availableAgainst()` on any book thin or gapped
enough that the next occupied level isn't adjacent.

**How:** Followed the plan's "measure → change one thing → measure" order strictly: T0's
benchmark first (so T3 would be measurable at all), then the cheap proof-hole closures (T1),
then the pure reorder (T2), then the one substantive algorithmic change (T3), then the
non-hot-path hardening (T4). Ran `latency-reviewer` after both `engine/order.hpp` and
`book/level_map.hpp` (clean both times — no allocation, exception, lock, or virtual dispatch
introduced; confirmed the debug asserts are genuinely `NDEBUG`-gated) and a `correctness-verifier`
pass on the bitset specifically, which additionally wrote an adversarial standalone harness
exercising `numSlots_ == 1/64/65`, `idx == 0`/`idx == numSlots_-1`, repeated same-price
empty→refill cycles, and a scattered 50-level book drained top-down with a bitset-vs-`empty()`
cross-check after every step — all passed, no bug found. Rejected leaving the aligned-new gap
"since nothing over-aligned exists today" — the whole point of a structural proof is that it
still holds after someone adds something that does.

**Before/after** (macOS-arm64, no core isolation — see caveats in `/bench` output; three repeat
runs each, batched-HdrHistogram method):

| Scenario | p50 | p99 | p999 | max |
|---|---|---|---|---|
| Dense steady-state (existing `BM_SubmitRestingOrder`/Hdr pass) — before | 13 ns | 15 ns | 18 ns | — |
| Dense steady-state — after | 13-14 ns | 16 ns | 18-19 ns | 85-403 ns |
| Thin/gapped book (new `BM_SweepThinBook`/Hdr pass) — before | 80 ns | 90 ns | 151 ns | 194 ns |
| Thin/gapped book — after | 14 ns | 21-25 ns | 28-130 ns | 31-215 ns |

Honestly: the dense scenario shows **no measurable change**, exactly as predicted — it never hit
the linear scan, so there was nothing for the bitset to fix there, and it stays within the
committed baseline's p50 11 / p99 14 / p999 18 ns (well under the 20% regression gate). The thin
book scenario improved **p50 by ~6x and p99 by ~3.5-4x**; p999/max remain noisy run-to-run
(no core isolation on this dev machine, load average 2-3.5 throughout from concurrent tool use in
this session) but are consistently and substantially below the pre-change numbers across every
run measured. `summary.json` was **not** touched — these numbers are reported here, not promoted,
pending a deliberate `/perf-baseline` run on a quieter machine.

Verified: all 21 replay tests byte-identical (`git status` confirms zero golden files touched),
72 unit tests green (65 prior + `Structural.NoExceptionsNoRtti` +
`Structural.HotPathForbiddenConstructs` + 3 new `cache_test.cpp` cases + 4 new
`level_map_test.cpp` gapped/bitset cases), 14 invariant profiles green including the new I9 check,
`velox_alloc_check` at 0 bytes/op / 0 allocs/op across the widened 6-op workload.
