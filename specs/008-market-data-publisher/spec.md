# Spec 008 — Market-data publisher (L2/L3 + trade ticks)

**Status:** 📋 BACKLOG · **Phase:** C — Real exchange surface · **Depends on:** 007

## Scope

Publish incremental order-book updates (L2 price-level aggregates and L3 per-order) plus a trade-tick
stream to subscribers — entirely **off the hot path**.

## Behavior

- **L2 incremental** — after every matching cycle, emit level add/modify/delete for both sides (FR-31).
- **L3 per-order** — new / cancel / fill events, sufficient to rebuild the book **order by order**
  (FR-32).
- **Trade ticks** — `tradeId`, aggressor `orderId`, passive `orderId`, price, quantity, logical sequence
  (FR-15, FR-33).
- The publisher is an **off-hot-path consumer of the outbound ring**. It **never blocks the engine**
  (FR-35, NFR-15). If a subscriber is slow, the subscriber suffers — the engine does not.

## The definition of done that actually matters

> **A subscriber, starting from a clean state and consuming only the feed, can reconstruct a book
> byte-identical to the engine's.** (FR-34)

This is the only test that means anything here, and it is a genuinely strong one. It catches every
class of feed bug at once: a missed update, an out-of-order update, an aggregate computed wrong, a
level deleted but not announced, an off-by-one in a quantity. If the reconstructed book diverges by a
single order at a single price, the feed is broken — and you will know exactly which event did it,
because the streams are deterministic and diffable.

A feed that "looks right" in a UI is not a tested feed. A feed that rebuilds the book bit-for-bit is.

## Definition of Done

- [ ] A subscriber reconstructs the book **identically to engine state** from the feed alone (FR-34).
- [ ] Verified over a long randomized session, not a hand-picked 20-order sequence.
- [ ] The publisher **never blocks the matching thread** — prove it: run a deliberately slow subscriber
      and show the engine's p99 is unchanged.
- [ ] A slow/disconnecting subscriber degrades **only itself**. Define and implement the overrun policy
      (drop the subscriber and make it resync, vs. buffer unboundedly — the latter is not an option, it
      is just a slower way to die).
- [ ] Trade ticks match the engine's emitted trades exactly.

## ⚠️ Depends on the Spec 005 resolution

The outbound ring feeds **both** this publisher and the execution-report router. `03-system-design.md`
types it as SPSC while giving it two consumers, which is not a thing that works. Spec 005 must have
resolved this (two rings, or a Disruptor-style multi-consumer barrier with independent cursors). This
spec **consumes** that decision; it does not get to re-litigate it.

## Requirements satisfied

FR-31…FR-35 · NFR-15 (off-hot-path consumer) · CON-7 (read-only downstream)
