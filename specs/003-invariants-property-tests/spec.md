# Spec 003 — Order-book invariants & property-based test harness

**Status:** 📋 BACKLOG · **Phase:** A — Correct core · **Depends on:** 002

## Scope

Encode the four book invariants and assert them **after every single operation** across thousands of
randomized order schedules. This is the spec that turns "we tested it" into **"correctness is proven"**
(constitution Principle 2).

## Why this exists, and why it is not just more tests

Golden replay proves the engine does the right thing **on the sequences we thought of**. That is the
limit of example-based testing: it can only find bugs in cases you already imagined, and the bugs that
survive to production are precisely the ones nobody imagined.

Property testing inverts this. Instead of asserting *outputs* for known *inputs*, it asserts
**properties that must hold for all inputs**, then generates inputs adversarially. It finds the
schedule you would never have written by hand — the one where a level is emptied by a partial fill and
recreated in the same operation, or where an order is cancelled at the exact moment it becomes the
best price.

This is what makes the resume claim *"correctness is the deliverable"* defensible rather than a boast.

## The four invariants (FR-49)

Asserted after **every** operation, not sampled at the end (NFR-21):

1. **Quantity conservation.** Nothing is created or destroyed.
   `Σ(resting qty) + Σ(traded qty × 2) == Σ(submitted qty)` — traded quantity counts twice because
   every trade consumes it from **both** an aggressor and a passive order.
2. **Sequence monotonicity.** Global sequence numbers strictly increase. No gaps, no reuse.
3. **No crossed book.** After matching completes: `bestBid < bestAsk`, or one side is empty.
   (*During* a match the book is transiently crossed — that is what a match **is**. The invariant is
   checked at operation boundaries, not mid-loop. Getting this distinction wrong makes the invariant
   fire spuriously and get disabled, which is worse than not having it.)
4. **FIFO fairness.** Within a price level, orders fill in arrival order. Always.

Plus the structural invariants, which are easier to break and just as fatal:

5. `OrderIdMap` ↔ level maps are **mutually consistent**. Every order in a level is in the id map and
   points back at its level; every order in the id map is in exactly one level. **A dangling entry here
   is how orders get silently lost** — the single most likely bug in this data structure.
6. No empty `PriceLevel` is left in a level map.
7. `bestBidPrice`/`bestAskPrice` equal the actual best occupied level, or the sentinel.

## Definition of Done

- [ ] All invariants hold after every operation across **thousands** of randomized schedules (FR-49).
- [ ] The harness generates adversarial schedules, not just uniform-random ones: heavy cancel rates,
      orders clustered at one price, alternating crossing/non-crossing, levels repeatedly emptied and
      recreated, an order book driven to empty and refilled.
- [ ] **Failures shrink.** A failing schedule is automatically reduced to the minimal counterexample.
      A 10,000-order failure a human cannot read is a failure that will not get fixed; a 3-order one
      gets fixed the same day. **The shrinker is not a nice-to-have — it is what makes this spec
      useful.**
- [ ] Failures are **reproducible from a printed seed**. The harness is random; the engine is not.
- [ ] `ctest -L invariant` is wired as a CI gate.

## Requirements satisfied

FR-49 (the four invariants) · NFR-21 (asserted after every operation; one violation = hard failure) ·
NFR-22 (no order lost, duplicated, or double-filled under any schedule) · Principle 2

## Note on randomness

The randomness is in the **test harness generating schedules** — never in the engine, which stays
strictly deterministic (Principle 3). The seed goes in, the schedule comes out, and the same seed
always produces the same schedule. This is precisely what makes a property failure debuggable instead
of a ghost story.
