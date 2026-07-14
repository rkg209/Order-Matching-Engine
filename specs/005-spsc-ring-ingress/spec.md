# Spec 005 — Hand-rolled SPSC ring-buffer ingress & threading model

**Status:** 📋 BACKLOG · **Phase:** B — Latency engineering · **Depends on:** 004

## Scope

The lock-free single-producer/single-consumer ring buffer that feeds the single matching thread, and
the threading model around it. Producers (gateway/sequencer) and consumers (execution-report router,
market-data publisher) run off-thread. Backpressure is defined and enforced.

## Behavior

- **`CommandRingBuffer`** — hand-rolled SPSC, cache-line-padded head and tail, power-of-two capacity
  (default 65,536), busy-spin wait (`cpu_pause()` via the platform shim).
- Events on the ring are **pre-allocated flyweights**. The producer writes field values into the
  existing slot; **the object never crosses the thread boundary**, only its field values do. This is
  what makes the handoff allocation-free.
- **Backpressure:** ring full ⇒ the producer **stops reading its socket** ⇒ TCP flow control pushes
  back to the sender (FR-28). **Never drop silently.** A dropped order in a trading system is a
  correctness failure, not a performance trade-off.
- The matching thread is **pinned** where the platform allows it. On macOS-arm64 it cannot be —
  `pin_thread_to_cpu()` returns false and we **report that honestly** rather than pretending.

## ⚠️ Design conflict this spec must resolve

`planning/03-system-design.md` §1.6 declares the **outbound** ring as `SpscRing<OutboundEvent>` and
then gives it **two consumers** — the `ExecutionReportRouter` and the `MarketDataPublisher`.

**A genuine single-producer-single-consumer ring cannot do that.** SPSC correctness rests on there
being exactly one reader owning the read cursor; two readers sharing one cursor will each consume a
subset of the events and both will be wrong.

Two legitimate options, and this spec must pick one and record why:

1. **Two independent rings** — the engine publishes each event twice. Simple, obviously correct, costs
   a second write on the hot path.
2. **Disruptor-style multi-consumer barrier** — one ring, each consumer holds an independent cursor,
   the producer may not overwrite a slot until the slowest consumer has passed it. This is what LMAX
   actually does, it costs no extra hot-path write, and it is meaningfully more work to get right.

Option 2 is the more defensible interview answer *and* the more honest fit for a project that cites
LMAX. But it must not be chosen for that reason alone — **measure both**.

## Definition of Done

- [ ] Sustained throughput ≥ **1,000,000 orders/sec** end-to-end through the ring (NFR-7, NFR-8) —
      **not** book-in-isolation.
- [ ] End-to-end latency (ring claim → match complete) measured and within budget.
- [ ] **No contention** on the hot path: no locks, no false sharing (verify head/tail are on separate
      cache lines — measure it, do not assume the `alignas` worked).
- [ ] Backpressure verified: a full ring stops the producer reading, and **no order is dropped**.
- [ ] The outbound-ring conflict above is **resolved, implemented, and the choice justified** in
      `plan.md` and `progress_report.md`.
- [ ] All golden replay scenarios still byte-identical.

## Requirements satisfied

FR-26 (single producer per connection) · FR-28 (backpressure, never drop) · NFR-7 (≥1M/sec) ·
NFR-8 (measured through the ring, not in isolation) · NFR-13/14 (single writer, no locks) ·
NFR-16 (false-sharing padding) · CON-3 (hand-rolled ring is mandatory — no mutex-guarded `std::deque`)

## Honesty note

Throughput here is measured in `bench` mode, which **skips the journal**. That is what makes ≥1M/sec
physically possible at all: `fsync` costs tens to hundreds of microseconds, so a million per second
cannot happen on any storage device that exists. **This number must always be reported as a
no-durability number** (constitution Principle 6). The durable path's throughput is a separate, lower,
separately-measured figure — do not blend them.
