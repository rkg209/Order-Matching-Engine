# Spec 006 — Sequencer + journal + deterministic recovery (event sourcing)

**Status:** 📋 BACKLOG · **Phase:** C — Real exchange surface · **Depends on:** 005

## Scope

Append every inbound command to a durable, segmented journal with a global monotonic sequence number,
take periodic snapshots, and rebuild **exact** state by replay on restart.

## The idea worth understanding

Determinism + a journal of every command ⇒ **exact replay**. And exact replay gives you **two** things
from **one** mechanism:

1. **Crash recovery** — restart, replay, and you are byte-identically where you were.
2. **Reproducible correctness tests** — the golden replay suite (Specs 001–003) *is* this same
   mechanism, pointed at a test fixture instead of a crash.

That is why determinism is a constitutional principle rather than a nice property. It is not one
feature; it is the foundation two different features stand on.

## Behavior

- **Sequencer** assigns a global monotonic sequence number **before** the engine sees the command
  (FR-16). This is the authoritative ordering — the engine's job is to be a deterministic function of
  this sequence.
- **Journal**: segmented, append-only. Record = `[4B length][8B globalSeq][1B commandType][N B payload]`.
  Segments roll at 256 MB.
- **`fsync` before the command is acked as sequenced** (FR-18, NFR-23). Durability precedes
  acknowledgement — otherwise you can ack an order and then lose it, which is the one failure a
  trading system may never have.
- **Snapshots** every 100,000 commands, to bound replay time. CRC32, atomic rename, retention 3.
- **Recovery**: load the latest *valid* snapshot, replay the journal tail from there (FR-20).

## ⚠️ Two conflicts this spec must resolve

**1. The snapshot mechanism contradicts itself in the planning docs.**
`03-system-design.md` §1.3 says a dedicated snapshot thread receives a **deep copy** of engine state
and the matching engine is *not paused*. But §2.6 says the matching engine **checks an atomic flag and
serializes its own state** into a buffer.

The second is a direct violation of the hot path: serializing the entire book inline would allocate,
would perform I/O-shaped work, and would blow the p99 budget by orders of magnitude — a 100,000-order
book cannot be walked in 20 µs. **The engine must not serialize its own state.** Design a copy-out or
copy-on-write handoff that keeps the matching thread's contribution O(1) — and if that proves
impossible, the honest answer is a brief, *measured*, explicitly-reported pause, not a pretence.

**2. `fsync`-per-record cannot coexist with 1M orders/sec.** These are physically irreconcilable. State
the resolution plainly:
- The **durable** path (live mode, journal in the loop) has a throughput ceiling set by storage, and it
  is measured and reported separately.
- The **1M orders/sec headline** is `bench` mode, which skips the journal, and is a **no-durability**
  number.
- If group-commit/batched-fsync is added to raise durable throughput, it **changes the durability
  guarantee** (a batch can be lost) and that must be stated, not buried.

## Definition of Done

- [ ] Kill the engine **mid-stream with `SIGKILL`** (not a graceful shutdown — a recovery mechanism that
      only survives clean shutdown is a save button, not recovery), restart, replay ⇒ **byte-identical**
      state (FR-50, NFR-24).
- [ ] Replay reproduces **all** trades deterministically, in the same order, with the same trade ids.
- [ ] A **torn tail record** (killed mid-`fsync`) is detected and discarded cleanly. No crash, no garbage.
- [ ] A **half-written snapshot** is never loaded (this is what CRC32 + atomic rename are for).
- [ ] Recovery from an **empty** journal works.
- [ ] Recovery needs **no manual intervention** (NFR-24). If a human runs a repair tool, it failed.
- [ ] Durable-path throughput measured and reported **separately** from the bench-mode headline.

## Requirements satisfied

FR-16…FR-21 · FR-50 · NFR-17 (byte-identical replay) · NFR-23 (durable before ack) · NFR-24 ·
NFR-25 (bounded replay distance) · Principle 3

## Missing from the planning docs

`04-database-design.md` was **truncated before §8**, which was to define the **snapshot binary
format**. It does not exist. This spec must design it: field order, endianness, versioning, and the
CRC32 placement. Do not assume a format exists to be copied.
