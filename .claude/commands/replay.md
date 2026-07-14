---
description: Run a deterministic golden-replay scenario and assert byte-for-byte trade equality.
allowed-tools: Bash, Read
---

Run the golden replay for scenario **"$ARGUMENTS"** (default: every scenario).

Available scenarios:
!`ls tests/replay/scenarios/ 2>/dev/null || echo "(none yet — scenarios land with Spec 001)"`

Steps:

1. Build, then run `ctest --test-dir build -L replay --output-on-failure`.
2. For each scenario: feed the fixed input command sequence through the engine and compare the
   produced trades and execution reports **byte-for-byte** against the committed reference file
   (FR-47). Not "equivalent" — identical bytes.
3. On failure, do not just report "replay failed". Find and report:
   - the **first** diverging record, by global sequence number,
   - the expected bytes vs the produced bytes at that record,
   - the minimal input prefix that reproduces the divergence.

   Determinism is what makes this possible: the same input *always* produces the same output, so a
   divergence is exactly reproducible and there is no excuse for an imprecise bug report.
4. A replay failure means the engine's observable behavior changed. If the change was intentional,
   the reference file must be regenerated **deliberately and reviewed** — never silently. If it was
   not intentional, you have found a real bug.

Golden replay is a mandatory CI gate (NFR-20). It must be green before anything is called done.
