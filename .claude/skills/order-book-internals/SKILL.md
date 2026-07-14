---
name: order-book-internals
description: The exact order-book data structure and the invariants that must hold after every operation. Use when touching the order book, price levels, the order-id map, or the bid/ask level maps — i.e. anything in engine/ or book/ that mutates book state.
---

# Order-book internals

## The structure (settled — do not redesign it)

Three cooperating pieces, chosen so that **every** operation the matching engine performs is O(1) or
best-price-fast:

```
BidLevels  (price DESC)          AskLevels  (price ASC)
  open-addressing map              open-addressing map
  int64 price -> PriceLevel*       int64 price -> PriceLevel*
  + bestBidPrice tracked as a      + bestAskPrice tracked as a
    field, updated on every          field, updated on every
    insert/remove                    insert/remove

        each PriceLevel = intrusive doubly-linked FIFO of Order nodes
        head ──> [Order] <──> [Order] <──> [Order] <── tail
                (earliest)                 (latest)

OrderIdMap: open-addressing map, int64 orderId -> Order*   ← this is what makes cancel O(1)
```

`Order` is a pooled flyweight with the intrusive `prev`/`next` pointers **inside it** — the list nodes
*are* the orders. No separate node allocation, no `std::list`.

## Why this and not something else (you will be asked)

- **Why not a heap/priority queue?** A heap gives you O(log n) best-price, but **cancel is O(n)** —
  you cannot find an arbitrary order to remove it. Real order flow is dominated by cancels (often
  >90% of messages), so optimizing match at the cost of cancel is exactly backwards.
- **Why not `std::map<price, level>`?** O(log n) lookup, a cache-hostile red-black tree, and a node
  allocation per new price level. The open-addressing map with a tracked best price gives O(1)
  average lookup and zero allocation.
- **Why track best bid/ask as a field instead of computing it?** Because it is read on *every* order
  and changed only when a level is created or emptied. Compute-on-write, not compute-on-read.
- **Why intrusive rather than `std::list<Order>`?** `std::list` allocates a node per insert. The
  intrusive list allocates nothing — the pointers live in the pooled `Order` itself.

## Operations, and their exact cost

| Op | How | Cost |
|---|---|---|
| Best bid / best ask | Read the tracked field | O(1) |
| Insert resting order | `OrderIdMap` insert + `PriceLevel` tail enqueue (+ create level if new) | O(1) |
| Cancel | `OrderIdMap` lookup → unlink from the intrusive FIFO by its own pointers | **O(1)** |
| Match | Walk from the `PriceLevel` head (earliest first) | O(fills) |
| Cancel/replace | Cancel, then insert as new — **time priority is reset** (FR-10) | O(1) |

**Cancel/replace resets time priority. This is deliberate and it is the correct market semantic** — a
modified order goes to the back of the queue at its price level. Otherwise a participant could hold a
front-of-queue position and repeatedly modify quantity to keep it, which is not fair and is not what
real exchanges do.

## Price-level lifecycle

- A level is **created** when the first order rests at that price.
- A level is **destroyed** the moment it becomes empty. Do not leave empty levels in the map — they
  make best-price tracking wrong and waste cache.
- When the level at the best price is destroyed, **the best price must be recomputed** — this is the
  one non-O(1) moment in the design, and it is why the sentinel values below matter.

## Sentinels (get these right or everything breaks)

- Empty bid side ⇒ `bestBidPrice == INT64_MIN`
- Empty ask side ⇒ `bestAskPrice == INT64_MAX`

These are chosen so the crossing test `bestBid >= bestAsk` is **naturally false on an empty book with
no special-casing**. `INT64_MIN >= INT64_MAX` is false; a bid against an empty ask book is
`price >= INT64_MAX`, false; an ask against an empty bid book is `price <= INT64_MIN`, false. The
empty-book case falls out of the arithmetic instead of needing a branch. Do not "fix" these to 0.

## Invariants — assert these after EVERY operation

Not sampled. Not at the end. After every single mutation, in tests (NFR-21):

1. **Quantity conservation.** Nothing is created or destroyed. Sum of resting quantity + sum of
   traded quantity == sum of submitted quantity.
2. **Sequence monotonicity.** Global sequence numbers strictly increase.
3. **No crossed book.** After matching completes, `bestBid < bestAsk`, or one side is empty.
   (*During* a match the book is transiently crossed — that is the whole point of a match. The
   invariant is checked at operation boundaries, not mid-loop.)
4. **FIFO fairness.** Within a price level, orders fill in arrival order. Always.

Plus the structural ones, which are just as important and easier to break:

5. `OrderIdMap` and the level maps are **mutually consistent** — every order in a `PriceLevel` is in
   the `OrderIdMap` and points back at its level; every order in the `OrderIdMap` is in exactly one
   `PriceLevel`. A dangling entry here is how orders get "lost", and it is the single most likely bug
   in this data structure.
6. No empty `PriceLevel` remains in a level map.
7. `bestBidPrice`/`bestAskPrice` equal the actual best occupied level (or the sentinel).

## Known trap

`planning/03-system-design.md` contains matching-loop pseudocode with a **real bug**: it always rests
into the bid book and always removes from the ask book, with **no branch on `order.side`**. A resting
SELL would land in the bids. **Do not copy that pseudocode.** Write the loop from the semantics in the
`matching-semantics` skill.
