---
description: Fail if the matching hot path allocates. Target is 0 bytes/op.
allowed-tools: Bash, Read, Grep
---

Prove the hot path allocates nothing at steady state (NFR-9, NFR-12).

Steps:

1. Run `./build/benchmark/velox_alloc_check`.

   It overrides global `operator new` / `operator delete` with counting versions, runs a warmup to
   reach steady state (pools filled, containers sized), then **zeroes the counters** and drives N
   orders through the matching path. Any allocation counted after the reset is a violation.

2. Report **bytes/op** and **allocations/op**. The target is **exactly 0** for both.

   Nonzero is a failure, not a "close enough". One allocation per order is one `malloc` per order,
   which is a lock, a potential syscall, and an unbounded tail — it is precisely the thing this
   project exists to avoid.

3. If it allocates, find *where*. Grep the hot path (`engine/`, `book/`) for the usual culprits:
   `new`, `malloc`, `std::vector` growth, `push_back` without `reserve`, `std::unordered_map`,
   `std::string`, `std::function`, lambda captures that heap-allocate, and any container that
   reallocates. Report file:line.

4. Remember the fix is never "allocate less" — it is **allocate never**: take the object from a
   pre-allocated `ObjectPool`, or pre-size the container at startup. When a pool is exhausted, the
   correct behavior is **backpressure, not a fallback allocation** (NFR-10). A fallback allocation
   is a hidden latency cliff, which is worse than a clean rejection.
