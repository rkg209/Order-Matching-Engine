# Spec 002 — Full order lifecycle & types

**Status:** ✅ COMPLETE · **Phase:** A — Correct core · **Depends on:** 001

## Scope

Widen the engine from "limit orders only" to the full order lifecycle: **market, IOC, FOK, cancel,
cancel/replace**, plus **self-trade prevention**. This is the correctness-breadth spec.

## Non-goals

Latency work (Spec 004). The property harness (Spec 003 — the *golden* scenarios land here, the
*randomized* ones there).

## Behavior

Read the `matching-semantics` skill. It is the normative statement; this is the summary.

| Type | Semantics |
|---|---|
| **MARKET** | Match against best available until filled or the book is exhausted. **Never rests.** Unfilled remainder is cancelled. |
| **IOC** | Match what it can immediately; **cancel the residual**. Partial fills expected. Never rests. |
| **FOK** | **All or nothing.** Entire quantity fills immediately, or the whole order is cancelled with **zero** fills. |
| **CANCEL** | By `orderId`. **Reject** if unknown or already fully filled — never silently succeed. |
| **CANCEL/REPLACE** | Atomic cancel + resubmit. **Time priority is reset** (FR-10). |

**Self-trade prevention:** an order must never match a resting order with the same `participantId`.
Policy: `CANCEL_AGGRESSOR` (default) / `CANCEL_PASSIVE` / `CANCEL_BOTH`. This is a **suppression, not a
skip** — do not step over the resting order and match the one behind it, which would violate price-time
priority by letting a later order fill ahead of an earlier one.

## The FOK design constraint (this is the interesting part)

**FOK cannot be implemented by matching incrementally and rolling back.** There is nothing to roll back
to: matching mutates destructively — levels are emptied, filled orders are unlinked and returned to the
pool. By the time you discover the order cannot be fully filled, you have already destroyed the state
you would need to restore.

FOK therefore requires a **two-pass** approach: walk the opposite book **without mutating anything**,
summing available quantity at acceptable prices; only if that sum ≥ the order quantity do you then
execute the match for real.

`planning/03-system-design.md`'s matching loop **cannot support FOK as written** — this was found
during planning, not during implementation, which is the entire point of writing specs first. Budget
for the pre-scan.

## Definition of Done

- [x] A golden replay scenario **and** a property test exists for **every** order type and edge case
      (FR-48's full list of 11): limit match · partial fill · full fill · cancel · cancel/replace ·
      market · IOC · FOK · crossing book · self-trade prevention · **market order into an empty book**.
      (`tests/replay/`, `tests/invariant/`.)
- [x] **No order is lost or double-filled under any scenario** (NFR-22). Quantity conservation holds —
      `Book.QuantityIsConservedAcrossMixedOrderTypes` + the randomized invariant suite.
- [x] Cancel of an unknown or already-filled order is **rejected**, with a reason.
      `Book.CancelUnknownIdIsRejected`, `Book.CancelOfAnOrderFullyFilledByThePrecedingCommandIsRejected`.
- [x] Cancel/replace **resets time priority** — proven by a test where the replaced order fills *after*
      an order that arrived later than the original but earlier than the replace.
      `Book.CancelReplaceResetsTimePriority`, `GoldenReplay.CancelReplaceResetsPriority`.
- [x] All Spec 001 golden scenarios still replay **byte-identically**. Adding order types must not
      change limit-order behavior. 21/21 `ctest -L replay` byte-identical.
- [x] `/bench` shows no p99 regression.

## Requirements satisfied

FR-6 (market) · FR-7 (IOC) · FR-8 (FOK) · FR-9 (cancel) · FR-10 (cancel/replace) · FR-13 (STP) ·
FR-14 (execution reports) · FR-48 (the 11 golden scenarios) · NFR-22 (no order lost or double-filled)

## Edge cases that must have tests

These are where matching engines actually break:

- **Market order into an empty book** — the naive implementation reads `bestAsk` from an empty book and
  either crashes or trades at `INT64_MAX`. Cheapest bug to write, most embarrassing to ship.
- **FOK that can *almost* fill** (needs 100, book has 99) — must fill **zero**, and must leave the book
  **completely untouched**.
- **Cancel of an order that was filled by the immediately preceding command** — a race in spirit, even
  though we are single-threaded.
- **Cancel/replace to a price that now crosses** — the replaced order must match, not rest.
- **STP where the aggressor would match multiple resting orders** and only *one* is a self-trade.
- **Market order that exhausts the book** with quantity still remaining — the remainder is cancelled,
  not rested.
