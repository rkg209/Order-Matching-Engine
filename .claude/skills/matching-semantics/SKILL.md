---
name: matching-semantics
description: The precise correctness rules for matching — price-time priority, limit/market/IOC/FOK semantics, partial fills, crossing books, and self-trade prevention. Use when implementing or altering any matching logic, or when writing a test that asserts what a match should produce.
---

# Matching semantics — the correctness spec

This is the document that decides what "correct" means. When code and this document disagree, the
code is wrong.

## Price-time priority (the core rule)

Among orders on the same side, the one that fills first is:

1. **Best price first.** For bids, the *highest* price. For asks, the *lowest*.
2. **Then earliest arrival.** Ties at the same price break by arrival order — strict FIFO, no
   exceptions, no size priority, no pro-rata.

That is the entire priority rule. Everything below is a consequence of it.

## Trade price: the resting order's price, always

When an incoming order crosses a resting order, **the trade executes at the RESTING order's price**,
not the incoming one.

This is the rule people get wrong most often, so be precise: a buy at 101 hitting a resting sell at
100 trades at **100** — the buyer gets price improvement of 1. The resting order was there first; it
set the terms, and it advertised a price it was willing to trade at. Honoring the aggressor's worse
price instead would let anyone extract value simply by quoting badly, and it would mean the displayed
book price is a lie.

## Crossing test

- A **buy** crosses when `buyPrice >= bestAsk`.
- A **sell** crosses while `sellPrice <= bestBid`.

Match repeatedly while the test holds and the incoming order still has quantity remaining. With the
`INT64_MIN`/`INT64_MAX` sentinels, an empty opposite book makes this test naturally false — no special
case needed.

## The order types

| Type | Semantics |
|---|---|
| **LIMIT** | Match while crossing. **Residual rests** in the book at its limit price. The workhorse. |
| **MARKET** | Match against the best available prices until filled or the book is exhausted. **Never rests.** Any unfilled remainder is cancelled. A market order has no price, so it cannot rest — there would be nothing to rest *at*. |
| **IOC** (immediate-or-cancel) | Match whatever it can, immediately. **Cancel the residual.** Never rests. Partial fills are fine and expected. |
| **FOK** (fill-or-kill) | **All or nothing.** Either the entire quantity fills immediately, or the whole order is cancelled with **zero** fills. |

### FOK needs a pre-scan — this is a real design constraint

FOK **cannot be implemented by matching incrementally and rolling back**, because there is nothing to
roll back to: matching mutates the book destructively (levels get emptied, orders get removed and
returned to the pool).

So FOK **must walk the opposite book first and sum the available quantity at acceptable prices,
without mutating anything**. Only if that sum >= the order quantity does it then execute the match for
real. Otherwise it is rejected untouched, having changed nothing.

`planning/03-system-design.md`'s matching loop does not do this and **cannot support FOK as written**.
This was flagged during planning. Budget for the two-pass walk in Spec 002.

## Partial fills

- An incoming order that only partly fills keeps its remainder — which **rests** (LIMIT) or is
  **cancelled** (MARKET/IOC/FOK).
- A **resting** order that is partly filled keeps its **original queue position** (FR-11). It does not
  go to the back of the line just because someone took part of it. Only a cancel/replace resets time
  priority.
- Every fill emits an execution report with `filledQty` and `remainingQty`. A partially filled order
  produces multiple reports, one per fill.

## Self-trade prevention (STP)

An order must never match against another order **from the same `participantId`**. A self-trade is a
wash trade: it moves no real risk, it fakes volume, and in real markets it is a regulatory violation.

When the matching loop would cross the aggressor into a resting order with the same `participantId`,
the configured STP policy fires:

- `CANCEL_AGGRESSOR` — cancel the incoming order (the default; the resting order was there first and
  keeps its position)
- `CANCEL_PASSIVE` — cancel the resting order, let the aggressor continue matching
- `CANCEL_BOTH` — cancel both

Note this is a **suppression, not a skip**. Do not silently step over the resting order and match the
one behind it — that would violate price-time priority (an order behind in the queue would fill ahead
of one in front of it). Fire the policy.

## Cancel and cancel/replace

- **Cancel** is by `orderId`. **Reject** if the id is unknown or the order is already fully filled —
  do not silently succeed. A cancel that "worked" on an order that already traded is how a client
  ends up with a position they think they cancelled out of.
- **Cancel/replace** (modify) is an atomic cancel + re-submit. **Time priority is reset** (FR-10): the
  replaced order goes to the back of the queue at its (possibly new) price level. See
  `order-book-internals` for why.

## Every one of these needs a test

Per FR-48, the golden replay suite covers, at minimum, these 11 scenarios — and a behavior without a
test does not exist (constitution Principle 2):

limit match · partial fill · full fill · cancel · cancel/replace · market order · IOC · FOK ·
crossing book · self-trade prevention · **market order into an empty book**

That last one is not padding. It is the case where a naive implementation reads `bestAsk` from an
empty book and either crashes or trades at a garbage sentinel price. It is the cheapest bug to write
and the most embarrassing one to ship.
