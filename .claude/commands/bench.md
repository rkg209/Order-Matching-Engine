---
description: Benchmark the engine and gate the result against the committed baseline.
allowed-tools: Bash, Read
---

Run the benchmark suite for scenario **"$ARGUMENTS"** (default: all scenarios).

Committed baseline:
!`cat benchmarks/baselines/summary.json 2>/dev/null || echo "NO BASELINE YET — this run will be the candidate for /perf-baseline."`

Hardware the baseline was measured on:
!`cat benchmarks/baselines/hardware.md 2>/dev/null | head -20 || echo "(hardware.md missing)"`

Steps:

1. Build in Release: `cmake --build build --config Release`.
2. Run `./build/benchmark/velox_bench` for the scenario.
3. Parse the HdrHistogram output for **p50, p99, p999, and throughput**. Report them in nanoseconds
   and orders/sec. Never report an average as a headline — p50/p99/p999 only (NFR-43).
4. Compare against the baseline above:
   - **p99 regression > 20% ⇒ FAIL LOUDLY.** State which scenario regressed and by exactly how much.
     This is a hard gate, not a warning (constitution Principle 1).
   - Also report p50 and p999 movement, but only p99 is the gate.
5. Regenerate `benchmarks/plots/latency-<scenario>.png` if the plotting script exists.
6. **Do NOT promote a new baseline.** That is `/perf-baseline`, a separate deliberate act.

Two honesty rules when reporting:

- Always state the hardware. A latency number without hardware context is meaningless (NFR-5).
- If this run used `VELOX_MODE=bench`, the throughput figure **skips the journal and gateway** and is
  therefore a **no-durability number**. Say so explicitly every time you report it (constitution
  Principle 6). Do not blend it with the durable-path number.

Finally, append an entry to `progress_report.md` if this run changed our understanding of the
project's performance — a regression found, a budget met, an optimization proven.
