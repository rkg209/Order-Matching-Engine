# Spec 009 — Benchmark harness & latency methodology (formalized)

**Status:** 📋 BACKLOG · **Phase:** D — Proof & demo · **Depends on:** 008

## Scope

**This is where the headline numbers are produced and locked.** Google Benchmark microbenchmarks, a
rate-driven load generator, HdrHistogram with coordinated-omission correction, a committed baseline
JSON, a CI regression gate, and the latency distribution plot that becomes the README's centerpiece.

Read the `benchmark-methodology` skill before touching anything here. It is the normative document.

## Coordinated omission — the thing this spec exists to get right

Spec 001's harness calls the book directly in a straight line, so there is no CO hazard — CO is a
property of **rate-driven** measurement. **This spec introduces the load generator, and with it the
hazard.**

The naive loop — `t0=now(); send(); t1=now(); record(t1-t0)` — is wrong, and wrong in the direction
that flatters you. When the system stalls, the loop stalls with it and simply **does not send** the
orders it should have sent during the stall. Those orders would have suffered the *worst* latency of
all — the entire stall plus their own service time — but they were never sent, so they were never
recorded. The benchmark omits precisely the samples that would have revealed the tail. A system that
freezes for 100 ms reports a beautiful p99, because during the freeze it recorded nothing.

**The fix:** measure against the **intended** schedule. If orders are meant to arrive every 1 µs,
latency runs from when the order *should have been sent*, not when we got around to sending it. Use
`hdr_record_corrected_value(hist, value, expected_interval)`. Using plain `hdr_record_value` in a
rate-driven loop **reintroduces the exact bug HdrHistogram exists to prevent** — and it is the easiest
mistake in this entire project to make, because the resulting numbers look great.

## Definition of Done

- [ ] Reproducible **p50/p99/p999 + throughput** on stated hardware (NFR-1…5).
- [ ] Load generator drives the **full end-to-end path** (gateway → ring → engine → exec report) at a
      configurable rate (FR-42), **not** book-in-isolation (NFR-8).
- [ ] Coordinated-omission correction is **on and verified** — deliberately inject a stall and confirm
      the tail *moves*. If an injected 50 ms stall does not show up in the p999, the correction is not
      working and every number is a lie. **Test the test.**
- [ ] Sample counts are large enough that the p999 is statistically real (millions, not thousands). If
      not, the p999 is **not reported**.
- [ ] `benchmarks/baselines/summary.json` committed, with `durable: false` set honestly.
- [ ] `benchmarks/baselines/hardware.md` records CPU, cores, RAM, OS, compiler, flags, and whether core
      isolation was in effect. **On macOS-arm64 it is not — say so.**
- [ ] CI fails on a **> 20% p99 regression** (NFR-6, NFR-34).
- [ ] The latency distribution plot is generated (FR-46) — this becomes the README centerpiece.

## The two numbers, and why they must never be blended

- **Throughput headline (≥1M orders/sec)** — `bench` mode. **Skips the journal and gateway.** This is
  what makes it physically possible: `fsync` costs tens to hundreds of microseconds, so a million per
  second cannot happen on any storage device that exists. This is a **no-durability** number and must
  be labeled as one, every single time (Principle 6).
- **Durable-path throughput** — live mode, journal in the loop, `fsync` before ack. Much lower. Measured
  and reported separately.

Both are legitimate measurements. Blending them is not. An interviewer who catches you claiming 1M/sec
*with* fsync durability has caught you not understanding your own system — and that is a far worse
outcome than the lower number would ever have been.

## Requirements satisfied

FR-41…FR-46 · NFR-1…NFR-8 · NFR-34 · Principle 1 · Principle 6

## Note on this machine

macOS-arm64 has **no core isolation** — no `taskset`, no `numactl`, no `SCHED_FIFO`. Numbers here are
honest numbers on a *shared* core. When a Linux box is available, re-measure with isolation, and
publish **both**, clearly labeled. Do **not** benchmark in Docker on Apple Silicon: it runs a VM and
the numbers are worthless.
