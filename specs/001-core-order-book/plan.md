# Spec 001 — Plan (the HOW)

## Module layout

Follows `02-architecture.md` §2.1, plus one addition (`platform/`) that the planning docs never
anticipated because they assumed Linux.

```
platform/     platform.hpp        cpu_pause() / pin_thread_to_cpu() / prefault_pages()
common/       object_pool.hpp     pre-allocated ObjectPool<T>, RAII acquire/release
              types.hpp           Price/Quantity/OrderId/Seq aliases, Side, sentinels
engine/       order.hpp           the pooled flyweight (intrusive prev/next live here)
              trade.hpp           the emitted trade
              price_level.hpp/cpp intrusive doubly-linked FIFO
              order_book.hpp/cpp  processNewOrder — the matching loop
book/         level_map.hpp/cpp   open-addressing int64 -> PriceLevel*, tracks best price
              order_id_map.hpp/cpp open-addressing int64 -> Order*
benchmark/    velox_bench.cpp     Google Benchmark + HdrHistogram (p50/p99/p999)
              velox_alloc_check.cpp  counting operator new; asserts bytes/op
tests/        unit/*.cpp          GoogleTest
              replay/*.cpp        golden scenarios, byte-for-byte
              replay/golden/*.txt committed reference outputs
```

## Data structures

**`Order`** — a flat, trivially-copyable struct. The intrusive list pointers live *inside it*, so the
list nodes **are** the orders: no separate node allocation, ever.

```cpp
struct Order {
    OrderId  id;            // int64
    Price    price;         // int64, scaled x10000
    Quantity quantity;      // int64, original
    Quantity remaining;     // int64, unfilled
    ParticipantId participant;  // int64 — unused until Spec 002 (STP), present now to avoid a later ABI churn
    Seq      seq;           // int64, arrival order — this IS time priority
    Side     side;          // uint8: BUY / SELL
    Order*   prev;          // intrusive FIFO
    Order*   next;
    PriceLevel* level;      // back-pointer, so cancel can unlink in O(1) (Spec 002)
};
```

Time priority is the **`seq` field, not a clock**. This is the determinism principle showing up as a
design decision: an arrival timestamp would make replay depend on wall-clock, so arrival *order* is
represented by a monotonic counter instead. Same semantics, deterministic by construction.

**`PriceLevel`** — head/tail pointers + aggregate quantity. `enqueue` at tail O(1); `unlink(Order*)`
O(1) given the node (which the id map hands us); match walks from `head` (earliest first).

**`LevelMap`** — open-addressing `int64_t price -> PriceLevel*`, power-of-two capacity, linear probing,
pre-sized at startup. Tracks `bestPrice` **as a field**, updated on insert/remove — because best price
is read on every order and changes rarely. Compute-on-write, not compute-on-read.

Sentinels: empty bids ⇒ `INT64_MIN`; empty asks ⇒ `INT64_MAX`. Chosen so the crossing test is
**naturally false on an empty book with no special case** — `bidPrice >= INT64_MAX` is false, and
`askPrice <= INT64_MIN` is false. The empty-book branch falls out of the arithmetic.

**`OrderIdMap`** — open-addressing `int64_t id -> Order*`. Built now, exercised in Spec 002.

## The matching loop (written from semantics, NOT from the planning pseudocode)

```
processNewOrder(order):
    opposite = (order.side == BUY) ? asks : bids          // ← branch on side. The planning
    same     = (order.side == BUY) ? bids : asks          //   pseudocode omits this. That is its bug.

    while order.remaining > 0 and opposite.hasBest():
        best = opposite.bestPrice()
        if not crosses(order, best): break                 // BUY: order.price >= best
                                                           // SELL: order.price <= best
        level = opposite.levelAt(best)
        while order.remaining > 0 and level.head != null:
            resting = level.head
            qty     = min(order.remaining, resting.remaining)

            emit Trade{ price: resting.price,              // ← RESTING price. Price improvement
                        qty,                               //   accrues to the aggressor.
                        aggressor: order.id,
                        passive:   resting.id }

            order.remaining   -= qty
            resting.remaining -= qty
            level.totalQty    -= qty

            if resting.remaining == 0:                     // fully filled -> leave the book
                level.unlink(resting)
                orderIdMap.erase(resting.id)
                pool.release(resting)
            // else: partially filled, KEEPS its position at the head. Do not re-enqueue.

        if level.empty():
            opposite.removeLevel(best)                     // recomputes bestPrice

    if order.remaining > 0:
        same.getOrCreate(order.price).enqueue(order)       // ← rests on its OWN side
        orderIdMap.insert(order.id, order)
```

The two places the planning doc gets wrong are marked. Both are the same mistake: not branching on
side.

## Latency harness (Principle 6 — from day one)

`velox_bench` builds a populated book, warms up until steady state, then times `processNewOrder` and
records into HdrHistogram_c. It reports **p50/p99/p999, never an average** (NFR-43).

For this spec the harness calls the book directly, so there is no rate-driven load generator and
therefore **no coordinated-omission hazard yet** — CO is a property of rate-driven measurement, and
this is a straight-line microbenchmark. The corrected recording path
(`hdr_record_corrected_value`) arrives with the load generator in **Spec 009**, and this is noted so
that nobody later assumes the CO problem was solved here when it was merely absent.

`velox_alloc_check` overrides global `operator new`/`delete` with counting versions, warms up, zeroes
the counters, then drives N orders and asserts bytes/op. **It is expected to report non-zero here** —
Spec 001 uses plain allocation for the book's setup path. Spec 004 is what drives it to zero. Recording
the honest starting number is the point: it is the "before" in a before/after we will have to defend.

## Trade-offs considered and rejected

- **`std::map` for price levels** — O(log n), cache-hostile red-black tree, a node allocation per
  level. Rejected: the open-addressing map with a tracked best price is O(1) and allocation-free.
- **A binary heap for best-price** — O(log n) best-price but **O(n) cancel**, because you cannot locate
  an arbitrary order. Real order flow is >90% cancels, so this optimizes the rare case at the expense
  of the common one. Rejected outright.
- **`std::list<Order>` for the FIFO** — allocates a node per insert. The intrusive list allocates
  nothing. Rejected.
- **Object pooling in this spec** — *deferred to Spec 004 on purpose.* Building the correctness suite
  first means the pooling change can be proven, via byte-identical golden replay, to have changed
  nothing about results. Doing it now would mean optimizing before we can detect a behavior change,
  which is exactly the failure mode the build order exists to prevent.

## Dependencies (CMake `FetchContent`)

GoogleTest · Google Benchmark · HdrHistogram_c. No vcpkg, no Conan — `cmake -B build` must work on a
clean machine.

## Risks

- **HdrHistogram_c is a C library** with a CMake build that is less well-behaved than the others. If
  `FetchContent` fights it, fall back to vendoring the two source files. Do not let this block the
  slice — but do not silently drop the histogram either, because then there is no latency number and
  Principle 6 is violated on the very first spec.
- **macOS-arm64 has no core isolation.** The first numbers will be noisier than a pinned Linux box
  would give. That is fine and it is honest, as long as `hardware.md` says so.
