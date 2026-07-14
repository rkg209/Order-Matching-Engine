---
name: benchmark-methodology
description: How to measure latency honestly — HdrHistogram, coordinated-omission avoidance, warmup, percentiles over averages, and the baseline JSON format. Use when writing or altering benchmarks, interpreting latency numbers, or reporting performance results.
---

# Benchmark methodology — how to not lie with numbers

This project's entire pitch is a **measured** latency number. That makes the measurement itself the
deliverable, and a dishonest measurement is worse than no measurement — it will be found, and in an
interview it will be found by the person you most wanted to impress.

## Never report an average

The average latency of a matching engine is a meaningless number. It is dominated by the fast common
case and completely hides the tail — and the tail is what actually hurts, because a trading system's
behavior is determined by its worst moments, not its typical ones.

**Report p50 / p99 / p999. Always.** (NFR-43.) If someone asks for "the latency", give them the
distribution. An average may appear in a table, never as a headline.

## Coordinated omission — the trap that invalidates most benchmarks

This is the single most important idea here, and the reason HdrHistogram is mandatory (NFR-4).

The naive benchmark loop is:

```
loop:  t0 = now();  send_order();  t1 = now();  record(t1 - t0)
```

This is **wrong**, and it is wrong in the specific direction that flatters you.

When the system stalls — a page fault, a scheduler preemption, a cache-cold path — the loop *also*
stalls, because it is synchronous. It simply does not send the orders it should have sent during the
stall. Those un-sent orders would have experienced the *worst* latency of all: they would have waited
out the entire stall plus their own service time. But they were never sent, so they were never
recorded.

The result: **the benchmark omits exactly the samples that would have shown the tail.** A system that
freezes for 100 ms will happily report a beautiful p99, because during the freeze it recorded nothing
at all. The measurement conspires with the stall.

**The fix:** measure against an *intended* schedule, not the actual one. Orders are supposed to arrive
at a fixed rate (say every 1 µs). Latency is measured from the time the order **should have been
sent** to the time it completed — not from when we actually got around to sending it. If a stall means
we sent 1,000 orders late, all 1,000 of them record their full lateness.

HdrHistogram_c does this with `hdr_record_corrected_value(histogram, value, expected_interval)`.
**Use the corrected variant.** Using plain `hdr_record_value` in a load-generator loop reintroduces
the exact bug HdrHistogram exists to prevent.

## Warmup, and what "steady state" means here

Measure only at steady state. Before the timed run:

- The object pools must be **filled and touched** — the first `Order` taken from a fresh pool touches
  a cold page and takes a page fault. That is a startup cost, not a matching cost.
- All containers pre-sized, all pages faulted in.
- The branch predictor and caches warm.
- The book must be **populated**, not empty. Matching against an empty book measures nothing — it is
  the trivial path. Load a realistic book with depth on both sides before timing.

Then, and only then, reset the histogram and start recording.

## Sample count — p999 needs to be real

A p999 computed from 1,000 samples is a single sample. It is noise, and it will swing wildly run to
run. To say anything meaningful about the 99.9th percentile you need **millions** of samples, so that
the tail bucket has thousands of observations in it.

If a benchmark run is too short to make the p999 statistically real, **do not report the p999**.
Report what you can defend.

## Hardware context is part of the number

A latency figure without stated hardware is not a result (NFR-5, DR-8). `benchmarks/baselines/hardware.md`
records: CPU model, core count, RAM, OS version, compiler and version, build flags, and whether core
isolation and frequency pinning were in effect.

**On this machine (macOS-arm64), core isolation is not available.** There is no `taskset`, no
`numactl`, no `SCHED_FIFO`, no way to keep the OS off the matching core. Say this plainly in
`hardware.md` rather than implying a pinned-core setup that does not exist. Numbers measured here are
honest numbers on a *shared* core; the pinned Linux numbers, when we have a Linux box, will be better
and will be labeled as such.

Do **not** benchmark inside Docker on Apple Silicon. It runs a VM, and the resulting latency numbers
are worthless.

## The throughput number is a no-durability number

`VELOX_MODE=bench` drives the engine directly, **skipping the journal and the gateway**. This is what
makes ≥1M orders/sec physically possible: `fsync` costs tens to hundreds of microseconds, so a million
`fsync`s per second cannot happen on any storage device that exists.

So the headline throughput figure is measured on a path **with no durability**. This is a legitimate
thing to measure — it is the matching core's capacity — but it **must always be reported as such**
(constitution Principle 6). The durable path has its own, much lower, separately-measured throughput.

Never blend the two. An interviewer who catches you claiming 1M orders/sec *with* fsync durability has
caught you not understanding your own system.

## The baseline JSON

`benchmarks/baselines/summary.json`:

```json
{
  "scenario": "steady_limit_orders",
  "p50_ns": 0, "p99_ns": 0, "p999_ns": 0, "max_ns": 0,
  "throughput_ops_sec": 0,
  "sample_count": 0,
  "durable": false,
  "mode": "bench",
  "hardware_ref": "benchmarks/baselines/hardware.md",
  "measured_at": "YYYY-MM-DD",
  "commit": "<sha>"
}
```

`durable: false` is a required field, precisely so the caveat above can never be lost.

The baseline is modified **only** by `/perf-baseline` (DR-7) — never automatically, and never as a
side effect of a change that made things slower. A gate that absorbs whatever the code does is not a
gate.
