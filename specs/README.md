# Velox — Spec Backlog

This directory **is a deliverable**, not scaffolding. Per CON-11, specs are never deleted after
implementation: a reviewer reading this repo can see the project was built spec-first, and can read
the reasoning behind every decision.

## How a spec is worked

Each spec directory contains up to three files, written at different times **on purpose**:

| File | Written | Contains |
|---|---|---|
| `spec.md` | Upfront, for every spec | The **WHAT** — observable behavior, DoD, requirements satisfied. No implementation. |
| `plan.md` | When the spec is **picked up** | The **HOW** — data structures, algorithms, trade-offs considered and rejected. |
| `tasks.md` | When the spec is **picked up** | The **STEPS** — phased, numbered, individually verifiable. |

`plan.md` and `tasks.md` are deliberately **not** written in advance. Planning a spec before the one
before it exists produces fiction: you plan against an imagined codebase, and the plan is wrong by the
time you reach it. Start a spec with `/spec NNN`.

## Build order (this order is not negotiable)

Correctness first, then latency, then the exchange surface, then proof, then depth. Latency work
(004–005) comes **after** the correctness suite (001–003) exists specifically so that every
optimization can be proven — via byte-identical golden replay — to have changed *nothing* about
results. Optimizing before you can detect a behavior change is how you ship a fast, subtly wrong
engine.

### Phase A — Correct core
- **000** — Constitution, CLAUDE.md, tooling, and a latency harness from day one
- **001** — Core limit order book + price-time matching *(the thin vertical slice)*
- **002** — Full order lifecycle & types (market, IOC, FOK, cancel, replace, STP)
- **003** — Order-book invariants & property-based test harness

### Phase B — Latency engineering (the headline)
- **004** — Zero-allocation, single-writer hot path
- **005** — Hand-rolled SPSC ring-buffer ingress & threading model

### Phase C — Real exchange surface
- **006** — Sequencer + journal + deterministic recovery (event sourcing)
- **007** — Binary wire protocol + order gateway
- **008** — Market-data publisher (L2/L3 + trade ticks)

### Phase D — Proof & demo
- **009** — Benchmark harness & latency methodology, formalized *(the headline numbers are produced and locked here)*
- **010** — Live visualizer (web)

### Phase E — Depth
- **011** — Multi-instrument & sharding *(optional; blocks nothing)*

### DEFERRED — optional, blocks nothing, no core spec may depend on these
- **012** — PostgreSQL audit tier ⚠️ *contradicts CON-8; see the spec*
- **013** — REST admin API ⚠️ *see the spec*

## Governance

Every spec is subordinate to `.specify/memory/constitution.md`. A spec that requires a hot-path
allocation, a lock in the engine, or a source of nondeterminism is **invalid** — it is reworked, not
granted an exception.
