---
name: low-latency-cpp
description: The zero-allocation rulebook for the matching-engine hot path (engine/, book/). Use whenever writing or modifying code in those modules, or when reviewing a change for hot-path violations, to avoid reintroducing allocations, locks, exceptions, virtual dispatch, or logging on the critical path.
---

# Low-latency C++ — the hot-path rulebook

The hot path is **`engine/` and `book/`**: everything the matching thread executes per order.
Everything else (`gateway/`, `sequencer/`, `marketdata/`, `visualizer/`) is off the hot path and may
allocate and log freely. **Know which side of the line you are on before you write a line.**

The rules below are not style preferences. Each one exists because violating it puts an unbounded
spike in the p999, and the p999 is the number this project is judged on.

## The forbidden list (hot path only)

| Forbidden | Why | Do this instead |
|---|---|---|
| `new`, `malloc`, `make_unique`, `make_shared` | An allocation is a lock, possibly a syscall, and an unbounded tail | Take from a pre-allocated `ObjectPool` |
| `throw` / exceptions | Throwing costs microseconds; even the *possibility* blocks optimizations and adds unwind tables | Return an error enum / status code |
| `virtual` | Indirect call, no inlining, a cache miss on the vtable | Templates, CRTP, or a plain `switch` on a type tag |
| `std::mutex`, `lock_guard`, `condition_variable` | Contention, priority inversion, syscalls. The engine is **single-writer** — there is nothing to lock against | Nothing. Design the contention away |
| `std::cout`, `printf`, spdlog | I/O on the hot path is a catastrophe | `alignas(64)` atomic counters, drained off-thread |
| `std::vector::push_back` (ungrown), `std::unordered_map`, `std::string`, `std::function` | All allocate and/or reallocate | Pre-sized arrays, open-addressing maps, function pointers or templates |
| `system_clock`, wall-clock | Breaks determinism (Principle 3) | Logical sequence numbers |
| `rand`, `<random>`, UUIDs | Breaks determinism | Deterministic ids from the sequencer |

**Carve-out:** `std::chrono::steady_clock` **is permitted**, and only for latency capture at cycle
boundaries. It never influences a matching decision, so it cannot affect determinism. This is stated
in the constitution so the tooling does not flag its own measurement apparatus.

## Zero allocation is achieved by construction, not by discipline

This is the key idea, and it is why the pool exists.

Do not write allocating code and then hunt allocations with a profiler. **Make allocation
structurally impossible**: every `Order` and `Trade` comes from an `ObjectPool<T>` sized at startup;
every event on the ring is a pre-allocated flyweight whose fields are overwritten in place; every
container is pre-sized before the first order arrives.

Then the profiler is not how you find allocations — it is how you *prove there are none*. Those are
very different activities, and only the second one scales.

**Pool exhaustion produces backpressure, never a fallback allocation** (NFR-10). A fallback
allocation is a hidden latency cliff that fires exactly when the system is under the most load —
i.e. at the worst possible moment. Rejecting the order cleanly is strictly better than serving it
slowly, because a rejection is bounded and a stall is not.

## Mechanical sympathy checklist

- **Cache lines are 64 bytes.** Keep the hot fields of `Order` in one line. Do not let a rarely-read
  field share a line with a hot one.
- **False sharing kills.** Any variable written by one thread and read by another gets `alignas(64)`.
  Two atomics on the same cache line will ping-pong it between cores and cost you more than the
  atomic itself. This is why the ring's head and tail are padded apart.
- **Branch predictability.** The matching loop's common case (no cross, or one fill) should be the
  fall-through path. Reach for `[[likely]]`/`[[unlikely]]` only with a measurement to back it.
- **Pointer chasing is a cache miss.** The intrusive FIFO is a linked list, which is normally a
  latency sin — it is acceptable here *only* because the nodes come from a contiguous pool arena, so
  they are usually already in cache. Do not add a second level of indirection on top.
- **`int64_t` prices, never floats.** Prices are scaled by 10,000. Floating point brings rounding
  error into money and nondeterminism into comparison. There is no float in the engine, anywhere.
- **Do not `#ifdef` in engine code.** Platform differences (`_mm_pause` vs `yield`, core pinning) live
  behind `platform/platform.hpp`. The engine must read as pure algorithm.

## Before you claim it is fast

Nothing here is true because it is written here. Prove it:

- `/alloc-check` — 0 bytes/op, or it is not zero-allocation.
- `/bench` — p50/p99/p999 against the budget, no regression.
- `/replay` — byte-identical. **Optimization must not change results.** If a latency change altered
  a single trade, it was not an optimization; it was a bug with a performance improvement attached.

That last point is the whole discipline in one sentence. The correctness suite (Specs 001–003) is
deliberately built *before* the latency work (004–005) precisely so that every optimization can be
proven to have changed nothing.
