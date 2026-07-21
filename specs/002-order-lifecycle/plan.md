# Spec 002 — Full order lifecycle & types — Implementation Plan

## Context

Spec 001 shipped a single-instrument, price-time-priority order book that handles **LIMIT orders
only** (`engine/order_book.cpp::submit`). The data structures were deliberately built wider than
001 needed: `book::OrderIdMap` exists but is only written to, `Order::level` back-pointer exists but
is never used for cancellation, and `Order::participant` is carried but never compared. Spec 002 is
the spec that cashes all three in.

The goal is correctness breadth, not latency: widen the engine to **MARKET / IOC / FOK / CANCEL /
CANCEL-REPLACE + self-trade prevention**, with a golden replay scenario and a unit/property test for
every one of FR-48's 11 cases. The hard constraint from `specs/002-order-lifecycle/spec.md` and the
`matching-semantics` skill is that **FOK cannot roll back** — matching mutates destructively (levels
emptied, orders unlinked and returned to the pool), so FOK needs a non-mutating pre-scan pass.

Two outcomes gate this work: all seven Spec 001 goldens must still replay **byte-identically**, and
`/bench` must show no p99 regression against `benchmarks/baselines/summary.json`
(p50=11ns / p99=14ns / p999=18ns).

---

## Current state (from exploration)

| File | Role today | Change needed |
|---|---|---|
| `engine/order_book.hpp` | `NewOrder`, `SubmitStatus`, `OrderBook` | add `OrderType`, `StpPolicy`, `OrderResult`, `cancel()`, `replace()` |
| `engine/order_book.cpp` | `submit()` — one match loop, rests residual | refactor into shared match loop + per-type residual disposition; add STP check; add FOK pre-scan |
| `engine/price_level.hpp` | `enqueue` / `unlink` / `reduceQuantity`, all O(1) | **no change** — `unlink()` is already exactly what cancel needs |
| `book/order_id_map.hpp` | `insert`/`find`/`erase`, backward-shift deletion | **no change** — `find`+`erase` are the cancel path |
| `book/level_map.hpp` | direct-indexed levels, `best()`, `onLevelEmptied()` | add one **const** helper: `nextOccupied(Price from)` for the FOK pre-scan walk |
| `common/object_pool.hpp` | `acquire`/`release`, no fallback alloc | **no change** |
| `tests/replay/replay_test.cpp` | parses `NEW …`, runs, serializes trades + book line | extend the command grammar; keep output format byte-stable |

---

## Design

### 1. Order types and results (`engine/order_book.hpp`)

```cpp
enum class OrderType : std::uint8_t { Limit = 0, Market, Ioc, Fok };
enum class StpPolicy : std::uint8_t { CancelAggressor = 0, CancelPassive, CancelBoth };
```

`OrderType type = OrderType::Limit;` is appended to `NewOrder` with a default, so every existing
call site and both existing test harnesses compile unchanged — this is what keeps the 001 goldens
byte-identical by construction rather than by inspection.

`StpPolicy stp = StpPolicy::CancelAggressor;` goes on `BookConfig`.

Execution reporting (FR-14) is an **enriched return struct**, not a second output stream. A full
`ExecutionReport` ring belongs in Spec 007/008 where the gateway and market-data consumers exist;
adding one now would be a channel with no reader.

```cpp
struct OrderResult {
    SubmitStatus status;    // Ok / Rejected* / Cancelled*
    Quantity     filled;    // total quantity that traded on this command
    Quantity     remaining; // unfilled at the end of the command
    bool         rested;    // did any quantity join the book?
};
```

`SubmitStatus` gains: `RejectedUnknownOrder`, `RejectedMarketIntoEmptyBook`, `RejectedFokUnfillable`,
`CancelledBySelfTradePrevention`, `CancelledResidual` (IOC/MARKET with an unfilled remainder — an
informational terminal state, not an error).

`submit()` keeps returning `SubmitStatus` **and** gains an `OrderResult` overload; the existing
signature stays as a thin wrapper so `tests/unit/order_book_test.cpp` and `replay_test.cpp` do not
have to change shape.

### 2. Market orders — no price, ever

A market order has no limit price, so `in.price` must never be read. Two consequences:

- **Validation:** the `bids_.inRange(in.price)` check in `submit()` is skipped for `Market`.
  Reading a garbage price through `LevelMap::index()` would index out of bounds.
- **Crossing:** the predicate becomes

```cpp
inline bool crosses(OrderType t, Side side, Price price, Price restingPrice) noexcept {
    if (t == OrderType::Market) return true;   // any resting price is acceptable
    return side == Side::Buy ? (price >= restingPrice) : (price <= restingPrice);
}
```

The empty-book case is still handled by the existing `opposite.hasBest()` loop guard, **not** by the
sentinel arithmetic — `crosses()` returning `true` unconditionally for Market is exactly why the
loop condition, not the price comparison, must be the empty-book gate. This is the "market order into
an empty book" bug the spec calls out: result is `RejectedMarketIntoEmptyBook`, zero trades, book
untouched.

### 3. One match loop, four residual policies

Refactor the body of `submit()` into a private:

```cpp
MatchOutcome matchInto(const NewOrder& in, Seq mySeq, TradeBuffer& trades,
                       Quantity& remaining) noexcept;
```

It is the current `while (remaining > 0 && opposite.hasBest())` loop verbatim, plus the STP check,
returning why it stopped (`Exhausted` / `NoLongerCrosses` / `StpFired`). The **only** thing order
type decides after the loop is the residual disposition:

| Type | Residual disposition |
|---|---|
| `Limit` | rest it (existing `pool_.acquire()` + `own.addOrder()` + `idMap_.insert()` path) |
| `Market` | cancel — `CancelledResidual` |
| `Ioc` | cancel — `CancelledResidual` |
| `Fok` | unreachable: the pre-scan guarantees residual is 0 |

Keeping the limit path structurally identical is what protects the 001 goldens and the p99: LIMIT
gains exactly one predictable branch (`type == Limit`) plus the STP integer compare, on a cache line
already loaded.

### 4. FOK — two-pass, pre-scan first

```cpp
Quantity availableAgainst(const NewOrder& in) const noexcept;
```

Walks the opposite `LevelMap` from `best()` outward using the new const `nextOccupied()`, and for each
level walks the intrusive FIFO from `head()`, summing `remaining` while `crosses()` holds. It mutates
nothing — no unlink, no `pool_.release()`, no `onLevelEmptied()`.

**STP interacts with the pre-scan and this is the subtle part.** Under the default
`CancelAggressor`, the first same-participant resting order the aggressor would reach terminates
matching entirely. The scan must therefore **stop summing at that order** rather than skipping past
it — otherwise FOK would report liquidity it can never legally reach and then fail mid-execution,
which is precisely the destructive-mutation situation FOK exists to avoid. Under `CancelPassive` the
scan skips it (it will be removed, and the queue behind it becomes reachable). This asymmetry gets an
explicit comment and a dedicated test.

Only if `availableAgainst(in) >= in.quantity` do we call `matchInto()`. Otherwise: return
`RejectedFokUnfillable` with zero trades and a provably untouched book (`needs 100, book has 99`
edge case).

`LevelMap::nextOccupied(Price from) const` is the same directional array walk `onLevelEmptied()`
already performs, factored out and made const. Both call it — no duplicated walk logic.

### 5. Cancel — O(1), reject-on-unknown

```cpp
OrderResult cancel(OrderId id) noexcept;
```

`idMap_.find(id)` → nullptr ⇒ `RejectedUnknownOrder`. Because a fully-filled order is already
`erase()`d and released in the match loop, "already fully filled" and "unknown" are **the same
lookup** — no separate filled-order table, and the spec's "cancel of an order filled by the
immediately preceding command" edge case falls out for free. On hit: `o->level->unlink(o)`, then
`onLevelEmptied(o->price)` **if the level is now empty**, then `idMap_.erase(id)`, then
`pool_.release(o)` — in that order, because `unlink()` reads `o->remaining` and `release()` invalidates
the object.

A partially filled resting order **is** cancellable (it is still in the map with `remaining > 0`);
the result reports its filled-to-date quantity.

### 6. Cancel/replace — validate, then cancel, then resubmit

```cpp
OrderResult replace(OrderId oldId, const NewOrder& fresh) noexcept;
```

Order of operations is load-bearing for atomicity: **validate everything first** (old id exists, new
id not a duplicate, new quantity > 0, new price in range) and return a rejection with the book
completely untouched if any check fails. Only then `cancel(oldId)` followed by `submit(fresh)`.

Time priority resets because `submit()` takes `++seq_` — the replacement is a genuinely new arrival.
The DoD proof case: rest A, rest B (later), replace A → A must fill *after* B.
"Replace to a price that now crosses" works with no special handling, because the resubmit is an
ordinary `submit()` that matches before it rests.

### 7. Self-trade prevention — suppression, not skip

Inside the inner FIFO loop, **before** emitting the trade:

```cpp
if (resting->participant == in.participant) { /* fire policy */ }
```

- `CancelAggressor` (default) — stop matching immediately, do not rest the residual, return
  `CancelledBySelfTradePrevention`. Trades already emitted this command stand.
- `CancelPassive` — unlink + erase + release the resting order, emit no trade, **continue** the loop.
- `CancelBoth` — do both.

Under no policy do we step over the resting order and match the one behind it. That would let a later
arrival fill ahead of an earlier one at the same price, breaking the one invariant the whole book is
built around.

---

## Files to change

- `engine/order_book.hpp` — `OrderType`, `StpPolicy`, `OrderResult`, extended `SubmitStatus`,
  `NewOrder::type`, `BookConfig::stp`, `cancel()`, `replace()`, private `matchInto()` /
  `availableAgainst()` / `restResidual()`.
- `engine/order_book.cpp` — the refactor above. This is the bulk of the work.
- `book/level_map.hpp` — add const `nextOccupied(Price from)`; refactor `onLevelEmptied()` to use it.
- `tests/replay/replay_test.cpp` — extended scenario grammar (below) + the new `TEST(GoldenReplay, …)`
  cases. **Serialization format is unchanged** for `TRADE`/`BOOK` lines; new line kinds
  (`CANCEL <id> OK|REJECT <status>`, `REPLACE …`) only ever appear in scenarios that use them.
- `tests/replay/scenarios/*.txt` + `tests/replay/golden/*.golden` — new scenarios only; **the seven
  existing pairs are not touched**.
- `tests/unit/order_book_test.cpp` — new unit tests, incl. a quantity-conservation helper.
- `tests/CMakeLists.txt` — only if a new test translation unit is added (prefer not to).
- `progress_report.md` — a `## [NNN]` entry (MANDATORY RULE 2).

### Scenario grammar (backward compatible)

```
NEW     <id> <BUY|SELL> <price> <qty> <pid> [IOC|FOK]   # bare NEW == LIMIT, parses as today
MARKET  <id> <BUY|SELL> <qty> <pid>
CANCEL  <id>
REPLACE <oldId> <newId> <price> <qty>
```

The existing parser already `continue`s on an unrecognized verb, so the 001 scenario files reparse
identically; the optional trailing token is read with `ss >> tok` which simply fails-and-clears on
the old lines.

---

## Test plan (the deliverable)

**New golden scenarios** — one per remaining FR-48 case:
`cancel` · `cancel_replace` · `market_order` · `ioc` · `fok` · `self_trade_prevention` ·
`market_into_empty_book`.

**Edge-case scenarios the spec names explicitly:**
- `fok_almost_fills` — needs 100, book has 99 ⇒ **zero** trades, and the `BOOK` line proves the book
  is bit-for-bit what it was before.
- `cancel_after_fill` — cancel an order the immediately preceding command fully filled ⇒ reject.
- `replace_into_cross` — replaced order matches instead of resting.
- `stp_multi_level` — aggressor would sweep three resting orders, only the middle one is a self-trade.
- `market_exhausts_book` — remainder cancelled, **not** rested (`resting=` count proves it).
- `cancel_replace_resets_priority` — the FR-10 proof: A rests, B rests later, A is replaced, B fills
  first.

**Unit tests** (`tests/unit/order_book_test.cpp`): per-type semantics, plus a
`conservation()` helper asserting `Σ resting remaining + Σ traded qty == Σ submitted qty` after
every scenario — this is NFR-22 ("no order lost or double-filled") made mechanical rather than
eyeballed.

Goldens are generated with `VELOX_BLESS=1` and then **read line by line before committing**. A
blessed-but-unread golden enshrines a bug and defends it forever.

---

## Verification

```bash
cmake -B build -G Ninja && cmake --build build

ctest --test-dir build -L unit        # incl. new lifecycle + conservation tests
ctest --test-dir build -L replay      # 7 old goldens byte-identical + ~13 new
ctest --test-dir build -L alloc_check # still 0 bytes/op — the FOK pre-scan must not allocate

./build/benchmark/velox_bench          # or /bench
```

Gates, in order:
1. **All seven Spec 001 goldens unchanged** — `git diff --stat tests/replay/golden/` must show only
   *added* files. Any modification to an existing golden is a failure, not a rebless.
2. `/replay` green, `/invariants` green.
3. `/alloc-check` — 0 bytes/op. The pre-scan walks existing memory; it must not touch the pool.
4. `/bench` — p99 within 20% of `benchmarks/baselines/summary.json` (p99=14ns). A LIMIT-order
   regression here means the type dispatch landed on the hot path badly; the fix is branch layout,
   not a baseline rebless.
5. Run the **`latency-reviewer`** sub-agent (mandatory after any `engine/` or `book/` edit) and the
   **`correctness-verifier`** sub-agent.
6. Append the `progress_report.md` entry.

## Suggested commit sequencing

1. Types + `OrderResult` + `nextOccupied()` (no behavior change; 001 goldens must already be green).
2. Match-loop refactor (still LIMIT-only behavior; goldens are the proof the refactor was neutral).
3. MARKET + IOC + their goldens.
4. FOK pre-scan + goldens.
5. Cancel + cancel/replace + goldens.
6. STP + goldens.
7. Conservation property helper + `progress_report.md`.

Step 2 is the risky one and it is deliberately isolated: if a golden diverges there, the cause is the
refactor and nothing else.
