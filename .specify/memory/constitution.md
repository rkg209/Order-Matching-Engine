# Velox Constitution

> The non-negotiable principles governing every spec, plan, task, and line of code in this project.
> Downstream artifacts (`specs/NNN-*/spec.md`, `plan.md`, `tasks.md`) inherit these. A spec that
> violates the constitution is rejected, not negotiated.

**Version:** 1.0 · **Ratified:** 2026-07-14

---

## Principle 1 — The performance budget is a hard gate

A change that regresses p99 beyond the committed budget is **rejected, exactly the way a failing
test is rejected**. Performance is not a follow-up ticket.

| Metric | Budget | Source |
|---|---|---|
| p50 order-to-match | ≤ 2 µs | NFR-1 |
| p99 order-to-match | ≤ 20 µs | NFR-2 |
| p999 order-to-match | ≤ 100 µs | NFR-3 |
| Throughput, single matching thread | ≥ 1,000,000 orders/sec | NFR-7 |
| Hot-path allocation, steady state | 0 bytes/op | NFR-9 |
| **p99 regression vs committed baseline** | **> 20% ⇒ hard build failure** | NFR-6 |

The committed baseline lives at `benchmarks/baselines/summary.json`. It is modified **only** by the
explicit `/perf-baseline` command (DR-7) — never as a side effect of a change that made things
slower. Promoting a baseline is a deliberate, reviewed act.

Every number is reported with its hardware context (`benchmarks/baselines/hardware.md`, DR-8). A
latency figure without stated hardware is meaningless and will not be published.

## Principle 2 — Correctness is proven, not claimed

Every order-type behavior and every edge case has a **golden replay scenario** (byte-for-byte trade
comparison against a reference file) and/or a **property test**. The book invariants are asserted
after *every* operation in tests, not sampled.

The four invariants (FR-49), which must hold after every single operation:

1. **Quantity conservation** — no quantity is created or destroyed across a match.
2. **Sequence monotonicity** — global sequence numbers are strictly increasing.
3. **No crossed book** — after matching completes, best bid < best ask (or the book is one-sided).
4. **FIFO fairness** — within a price level, orders fill in arrival order, always.

A behavioral capability without a test proving it does not exist. This is the meaning of
"correctness is the deliverable."

## Principle 3 — Determinism is mandatory

Same input journal ⇒ **byte-identical** output. Always.

Forbidden in the engine (`engine/`, `book/`): wall-clock reads (`system_clock`), randomness
(`std::rand`, `<random>`), UUIDs, hash containers with randomized seeds or nondeterministic
iteration order, and any dependence on thread scheduling.

**Carve-out (explicit, so tooling does not fight itself):** `std::chrono::steady_clock` is permitted
on the hot path, and *only* for latency capture at cycle boundaries. It never influences a matching
decision, so it cannot affect output determinism. The `latency-reviewer` agent must not flag it.

Determinism is not a nicety. It is the single mechanism that gives us *both* crash recovery *and*
reproducible correctness tests — one property, two payoffs.

## Principle 4 — Single-writer, zero-allocation hot path

Architectural law:

- The matching engine is **one pinned OS thread**. No other thread touches engine state. No
  `std::mutex`, no `std::lock_guard`, no concurrent containers on the hot path (NFR-13, NFR-14).
- Cross-thread handoff happens **exclusively** through the hand-rolled lock-free SPSC ring buffer.
- **Zero bytes allocated per operation** at steady state. No `new`, no `malloc`, no container
  reallocation (NFR-9, NFR-12). Orders and trades come from pre-allocated `ObjectPool`s;
  events on the ring are pre-allocated flyweights whose fields are overwritten in place.
- **Pool exhaustion produces backpressure, never an allocation** (NFR-10).
- No exceptions, no virtual dispatch, no logging on the hot path (NFR-30).
- False sharing is eliminated with `alignas(64)` (NFR-16).

**The single-threaded matching hot path is settled and SHALL NOT be re-litigated** (CON-2). Scaling
happens through per-instrument sharding (Spec 011), not through threading the engine.

## Principle 5 — Thin vertical slice first, then deepen

Every spec must be **demoable end-to-end before the next one begins**. We build a correct, narrow
thing and widen it; we never build three half-finished layers.

Build order is not negotiable either: correctness core (001–003) → latency discipline (004–005) →
exchange surface (006–008) → proof and demo (009–010) → depth (011). Latency work comes *after* the
correctness suite exists, precisely so that optimization can be proven to change nothing about
results.

## Principle 6 — Measure from day one

A latency harness exists from **Spec 001**, before there is anything worth optimizing. You never
optimize blind, and you never discover in month three that the number was always bad.

Measurement methodology is itself governed:

- **HdrHistogram_c**, with **coordinated-omission correction** (NFR-4). Averages are never the
  headline metric — p50/p99/p999 are.
- Pinned, stated hardware (NFR-5).
- Throughput measured **end-to-end through the ring buffer**, not book-in-isolation (NFR-8).

**Honesty carve-out (stated once, applies forever):** the `bench` startup mode skips the journal
and the gateway, driving the engine directly. This is what makes ≥ 1M orders/sec physically possible
— `fsync`-per-record cannot be done a million times a second on any storage device. Therefore the
throughput headline is a **no-durability number and must always be reported as such**. The durable
path has its own, lower, separately-measured throughput. We do not blur these two numbers together,
in the README or in an interview.

---

## Scope boundaries

**CON-8 governs:** the engine has **no external database and no persistent store**. The system is
in-memory; the segmented journal is the *sole* durability mechanism. The PostgreSQL design in
`planning/04-*` and the REST admin API in `planning/05-*` contradict this. They are **deferred** to
`specs/012-postgres-audit-tier/` and `specs/013-rest-admin-api/`, are optional, and block nothing.
No core spec may take a dependency on them.

**CON-11:** `specs/` is a committed portfolio artifact. Specs are **never deleted** after
implementation.

**CON-12:** `.claude/` (commands, agents, skills, hooks, settings) is committed. The guardrails
travel with the repo.

**CON-7:** the visualizer is strictly read-only. It sends nothing to the engine, gateway, or
sequencer, ever.

## Amendment

This document changes only by explicit decision, recorded as an entry in `progress_report.md` that
states what changed, why, and what it invalidates. Bump the version above.
