# Spec 004 — Zero-allocation, single-writer hot path

**Status:** 📋 BACKLOG · **Phase:** B — Latency engineering · **Depends on:** 003

## Scope

Drive hot-path allocation to **zero bytes per operation**. Object pools for orders and trades,
flyweight events, custom pool/arena allocators, open-addressing maps sized at startup, cache-friendly
layout, false-sharing padding. Remove every allocation, lock, and log from `engine/` and `book/`.

**This is the headline spec.** Everything before it made the engine correct; this is what makes it
fast.

## The defining constraint

**All Phase-A tests must remain byte-identical.** (Not "still pass" — *byte-identical*.)

This is the entire reason the build order puts correctness first. A latency optimization that changes
a single trade is not an optimization; it is a bug with a performance improvement attached. Because
Specs 001–003 built a golden replay suite that compares output byte-for-byte, we can **prove** that
this spec changed nothing about behavior — and that proof is what makes the latency work trustworthy
rather than merely fast.

If a golden file needs regenerating during this spec, **stop**. Something is wrong.

## Behavior

Nothing changes. Not one observable byte. That is the point.

## Definition of Done

- [ ] `/alloc-check` reports **0 bytes/op and 0 allocations/op** at steady state (NFR-9, NFR-12).
- [ ] **Every** Phase-A golden replay scenario is byte-identical to before this spec (FR-47).
- [ ] All invariants still hold across randomized schedules.
- [ ] p50/p99/p999 **measurably improved** vs the Spec-001 baseline. The improvement is the deliverable
      — record the before and after in `progress_report.md`, and if the improvement is small, say so
      instead of dressing it up.
- [ ] The `latency-reviewer` sub-agent reports zero violations in `engine/` and `book/`.
- [ ] No `std::mutex`, no locks, no exceptions, no virtual dispatch, no logging anywhere on the hot
      path.

## The work

- **`ObjectPool<Order>` / `ObjectPool<Trade>`** — pre-allocated at startup, RAII acquire/release.
  **Pool exhaustion produces backpressure, never a fallback allocation** (NFR-10). A fallback
  allocation is a hidden latency cliff that fires exactly when the system is under maximum load — i.e.
  at the worst possible moment. A clean rejection is bounded; a stall is not.
- **Flyweight events** — pre-allocated on the ring; the producer overwrites fields in place rather than
  constructing an object.
- **Pre-sized containers** — every map and array sized before the first order arrives. No growth, ever.
- **Cache layout** — hot fields of `Order` in one 64-byte line. `alignas(64)` on anything written by
  one thread and read by another (NFR-16). Two atomics sharing a cache line will ping-pong it between
  cores and cost more than the atomic itself.
- **Arena for `PriceLevel`s** — contiguous, so the intrusive list's pointer-chasing usually hits cache.
  A linked list is normally a latency sin; it is acceptable here *only* because the nodes are
  contiguous.

## Requirements satisfied

NFR-9 (0 bytes/op) · NFR-10 (pool exhaustion → backpressure) · NFR-11 (pre-sized containers) ·
NFR-12 (no `new`/`malloc`/realloc) · NFR-13 (single writer) · NFR-16 (false sharing) ·
NFR-30 (no hot-path logging) · Principle 4

## How to work this spec

**Measure, change one thing, measure again.** Do not do a big-bang rewrite — a rewrite that changes
eight things and improves p99 by 30% has taught you nothing about which of the eight mattered, and one
of them is probably a regression hiding behind the others.

Run `/replay` after **every** change. The moment output diverges, you have introduced a bug, and you
want to know that while the change is one edit old rather than fifty.
