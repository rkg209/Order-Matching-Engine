---
description: Deliberately promote the current benchmark numbers to the committed baseline. Guarded.
allowed-tools: Bash, Read, Write
---

Promote the current benchmark result to `benchmarks/baselines/summary.json`.

**This is a deliberate, guarded act — never a side effect.** The baseline is the regression gate. If
it silently absorbed every new number, it would ratchet to whatever the code happens to do and would
catch nothing. Per DR-7, this command is the *only* thing permitted to modify it (the Bash guard hook
blocks other writes).

Before promoting, you MUST:

1. **Confirm the intent is real.** Ask the user to confirm, and state plainly what is changing:
   the old p50/p99/p999/throughput vs the new ones, with the delta.

2. **If any metric got WORSE, stop and challenge it.** Promoting a regression erases the evidence of
   the regression. Ask explicitly: *is this a deliberate, accepted trade-off?* If the answer is not a
   clear yes with a stated reason, do not promote. Say no.

3. **Verify the run is trustworthy** before it becomes the number everything else is judged against:
   - Release build, optimizations on.
   - The full test suite is green (a fast, wrong engine is not a baseline).
   - The machine was quiet — no other significant load.
   - The run had adequate warmup and enough iterations for the p999 to mean anything. A p999 from
     1,000 samples is noise, not a tail.

4. **Record the hardware** in `benchmarks/baselines/hardware.md` (DR-8): CPU model, core count, RAM,
   OS version, compiler and version, build flags, and whether core isolation / frequency pinning was
   in effect. A number without this context cannot be compared to anything and must not be published.
   On macOS-arm64 there is no core isolation available — **say so** rather than implying there was.

5. **Say whether the journal was in the path.** A `VELOX_MODE=bench` throughput number skips the
   journal and gateway and is a **no-durability** number. Label it as such in the JSON itself.

After promoting, append an entry to `progress_report.md` recording the old numbers, the new numbers,
why they moved, and the hardware. The baseline's history is part of the project's story.
