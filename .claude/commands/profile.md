---
description: Profile the hot path and produce a flamegraph to find the next real bottleneck.
allowed-tools: Bash, Read
---

Profile scenario **"$ARGUMENTS"** (default: the standard matching load) and find where the time
actually goes.

Steps:

1. Build with symbols and optimizations: `-O2 -g -fno-omit-frame-pointer`.
2. Sample the matching thread:
   - **macOS:** `xctrace` / `sample`, or `dtrace`-based sampling. (Note: `perf` does not exist here.)
   - **Linux:** `perf record -g --call-graph=dwarf` then `perf script | flamegraph.pl`.
3. Produce the flamegraph and read it. Report the top frames **by self time** on the matching thread.

Then interpret it honestly, because a profile is easy to misread:

- Look for the things this project has already declared war on: allocation (`malloc`/`operator new`
  showing up at all is a bug, not a hotspot), locking, cache misses on the price-level or id-map
  lookups, branch misprediction in the matching loop.
- **A profile shows the mean, not the tail.** The p999 is our hardest budget, and a sampling profiler
  will barely see it. If the mean looks fine but p999 is bad, the profile is the wrong instrument —
  reach for the HdrHistogram distribution and look for what happens rarely: a page fault, a pool
  refill, a cache-cold path, a level being created or destroyed.
- **Do not optimize what the profile does not show.** State the measured cost of the bottleneck
  before proposing a fix, and re-measure after. `/bench` decides whether the fix worked — not
  intuition, and not the fact that the code now looks faster.
