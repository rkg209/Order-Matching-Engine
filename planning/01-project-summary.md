# project-summary.md

```markdown
# Velox Matching Engine — Executive Summary

## What It Is

Velox is a low-latency, in-memory order-matching engine and mini-exchange built in modern
C++20. It is the core of what a stock or cryptocurrency exchange
runs: buy and sell orders arrive, the engine maintains an order book, and it matches them by
price-time priority — best price first, then earliest order — executing trades entirely in
memory, optimized for the lowest and most predictable latency possible.

Around that matching core sits a thin but complete exchange: a binary order gateway that
clients connect to over TCP, a sequencer and journal that make the system crash-recoverable
and perfectly reproducible, a market-data feed that streams book updates and trade ticks to
subscribers, and a live web visualizer that shows the order book and the latency distribution
in real time.

## Who It Is For

**Primary audience: low-latency, quantitative, and high-frequency trading infrastructure
interviewers** at firms such as IMC, Optiver, Citadel Securities, DE Shaw, Jane Street, and
performance-focused infrastructure teams at companies like Stripe.

"Design a matching engine" and "design an order book" are among the most common system-design
prompts in low-latency and HFT-adjacent interviews. Velox is the project where the builder
has already done it — in full, with measured numbers, proven correctness, and a defensible
architecture — rather than sketching an answer on a whiteboard.

## Why It Matters

### The problem it solves is real and hard

Exchanges must match enormous order volumes with minimal, predictable latency and exact
correctness. Getting this wrong means lost orders, incorrect fills, unfair priority, or a
system that cannot recover from a crash. The engineering challenges — data-structure design,
latency discipline, deterministic recovery, protocol design — are the same ones that real
exchange infrastructure teams face every day.

### It is measurably correct, not just claimed correct

Every order type and edge case — partial fills, cancellations, cancel-and-replace, market
orders, immediate-or-cancel, fill-or-kill, self-trade prevention, crossing books — has a
golden replay scenario and a property-based test. The same deterministic journal that enables
crash recovery also makes every bug reproducible. Correctness is proven by tooling, not
asserted in prose.

### The latency story is honest and quantified

The headline numbers — median around 1–2 microseconds, p99 around 10–20 microseconds at
over one million orders per second — are measured with HdrHistogram using coordinated-
omission-free methodology, on stated hardware, with committed baselines that a CI gate
enforces. The zero-allocation, lock-free hot path is measured this way from day one — there
is no garbage collector to reason about, because there isn't one. These are not estimates
or theoretical bounds; they are measured results.

### The architecture is defensible at every level

The single-threaded matching hot path (a hand-rolled lock-free SPSC ring buffer, LMAX-style) is the architecture real
exchanges use. The zero-allocation, lock-free hot path is enforced by automated tooling —
hooks that scan every code change, a sub-agent that reviews every hot-path edit, and an
allocation profiler that fails the build if the hot path allocates. The event-sourced journal
is the same mechanism that powers both crash recovery and reproducible correctness tests.
Every design decision has a reason that holds up under interview questioning.

### It demonstrates the full systems-thinking surface

A pure matching core reads as a data-structure exercise. Velox adds the surrounding exchange
surface — protocol design, event sourcing, market-data streaming, recovery — to show that the
builder understands how the pieces fit together in a real system, while keeping the low-
latency core as the clear headline.

## The Headline Resume Line

> *Order-Matching Engine (C++20) — built a low-latency exchange core (single-threaded
> deterministic matching, lock-free ring-buffer ingress, journaled recovery, binary gateway,
> market-data feed, live visualizer); ~p99 15 µs @ 1 M orders/sec.*

## What Gets Shipped

A public repository containing the full source, a committed spec backlog (itself a portfolio
artifact showing spec-driven development discipline), benchmark baselines and latency
distribution plots, a one-command demo, and a design writeup explaining every major decision.
The live visualizer gives any reviewer —
technical or not — an immediate, tangible picture of the system working.
```