# Spec 011 — Multi-instrument & sharding

**Status:** 📋 BACKLOG (optional depth) · **Phase:** E · **Depends on:** 009 · **Blocks:** nothing

## Scope

One matching thread per instrument (sharded engines), each with an isolated order book and its own
ingress ring, pinned to its own core. The gateway decodes the instrument symbol and routes to the
correct shard.

## Why this is the *only* way we scale

CON-2 settles it: **the single-threaded matching hot path is not re-litigated.** We do not make the
engine multi-threaded, ever. Not because threading is hard, but because a multi-threaded matching
engine gives up the two properties the entire project is built on — determinism and predictable tail
latency — in exchange for a throughput increase you can get more cheaply and more safely by sharding.

So scaling is **horizontal, by instrument**: N instruments, N independent single-threaded engines, N
cores. Each shard keeps every guarantee the single engine had. Nothing is shared, so nothing needs
locking, so determinism survives per-shard and the tail latency does not degrade.

This is also the correct interview answer to *"okay, but how do you scale it?"* — and it is much
stronger for being the thing you actually built rather than the thing you would do given more time.

## Behavior

- One `OrderBook` + one `CommandRingBuffer` + one pinned matching thread **per instrument**.
- The gateway routes by `instrumentId` to the right ring (FR-51).
- Shards are **fully isolated**. No shared mutable state. An order for AAPL cannot touch the MSFT book.
- Per-instrument determinism is preserved **independently** (FR-52).

## Definition of Done

- [ ] N instruments run isolated; a fault or a slow consumer in one shard does not affect another.
- [ ] Aggregate throughput **scales with instrument count** — measure it and show the curve. If it does
      not scale roughly linearly up to the core count, something is shared that should not be, and
      finding that is the point of the measurement.
- [ ] Per-instrument determinism preserved: each shard replays byte-identically on its own.
- [ ] Per-instrument p99 is **no worse** than the single-instrument p99. If sharding degraded the tail,
      the isolation is not real.

## Requirements satisfied

FR-51 · FR-52 · CON-2 (scale by sharding, never by threading the engine)

## Honest limits to state

- **Cross-instrument atomicity does not exist here.** There is no way to atomically trade AAPL against
  MSFT — no basket orders, no cross-instrument risk checks. That is a real limitation of this design
  and it should be **stated**, not hidden. The exchanges that need it solve it with a different
  architecture, and knowing that is worth more than pretending the limitation is not there.
- Scaling is bounded by physical cores. Past that, you are time-slicing, and the tail latency you gave
  up threading to protect comes back anyway.
