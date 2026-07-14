---
description: Run the property-based invariant suite over thousands of randomized order schedules.
allowed-tools: Bash, Read
---

Run the invariant/property suite: `ctest --test-dir build -L invariant --output-on-failure`

This asserts the four book invariants **after every single operation** (FR-49, NFR-21), across
thousands of randomized order schedules:

1. **Quantity conservation** — no quantity is created or destroyed by a match. Total quantity in the
   book plus total traded quantity equals total quantity submitted.
2. **Sequence monotonicity** — global sequence numbers are strictly increasing, with no gaps or
   reuse.
3. **No crossed book** — once matching completes, best bid < best ask, or the book is one-sided.
4. **FIFO fairness** — within a price level, orders fill in arrival order. Always. No exceptions.

One violation is a **hard failure** — these are not statistical properties that mostly hold.

On failure:
- Report the **seed** of the failing schedule (the suite must print it — determinism means the seed
  fully reproduces the failure).
- **Shrink** the failing schedule to the minimal sequence of operations that still violates the
  invariant, and report that. A 3-order counterexample is worth a hundred times a 10,000-order one.
- State which invariant broke, and the exact book state at the moment it broke.

Note the randomness here is in the *test harness* generating schedules — never in the engine itself,
which stays deterministic (constitution Principle 3).
