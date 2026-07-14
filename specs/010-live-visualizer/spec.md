# Spec 010 — Live visualizer (web)

**Status:** 📋 BACKLOG · **Phase:** D — Proof & demo · **Depends on:** 009

## Scope

A read-only web app that renders the order book updating live and the latency histogram in real time,
fed by the market-data + latency stream over WebSocket.

## Why it exists

This is the **recruiter-facing artifact**. A p99 of 15 µs means nothing to a non-specialist; an order
book ladder animating in real time next to a latency histogram is immediately, viscerally legible to
anyone. It is the single most tangible thing a non-expert can grasp about this project — and it costs
the latency story **nothing**, because it is a pure downstream consumer that never touches the hot
path.

That last clause is the whole design. A demo that compromised the engine's latency to look good would
defeat its own purpose.

## Behavior

- **Animated order-book ladder** — top-N bid/ask levels with aggregate quantity, updating live (FR-37).
- **Real-time latency histogram** — p50/p99/p999, updated ≥ 1×/sec (FR-38, NFR-31).
- Fed over **WebSocket** (JSON) from the market-data and latency streams (FR-36).
- Works on a **replayed session** from a pre-recorded journal, not just live (FR-40) — so the demo is
  **deterministic and repeatable**. It shows the same thing every time, which matters when you are
  demoing to an interviewer over a video call and cannot afford a "well, it usually does something
  more interesting than this."
- **Strictly read-only** (CON-7, FR-39). It sends **nothing** to the engine, gateway, or sequencer.
  Ever. Not a subscribe message, not a heartbeat that reaches the engine.

## Definition of Done

- [ ] The book ladder renders and animates from the live feed.
- [ ] The latency histogram updates ≥ 1×/sec with real p50/p99/p999.
- [ ] It runs from a **replayed journal**, producing an identical demo every time.
- [ ] **Provably decoupled:** run the visualizer under load and show the engine's p99 is **unchanged**
      versus running without it. Do not assert the decoupling — *measure* it. "It shouldn't affect the
      hot path" is exactly the sort of claim that turns out to be false.
- [ ] The visualizer sends **zero bytes** toward the engine. Verify at the socket level, not by reading
      the code.
- [ ] Serves at a configurable port, default **8080** (DR-6). No browser plugin.

## Requirements satisfied

FR-36…FR-40 · NFR-31 (≥1 Hz update) · CON-7 (strictly read-only)

## Scope discipline

The temptation here is to build a beautiful trading UI. **Resist it.** Two panels — ladder, histogram
— done well beats six panels done adequately, and every hour spent on the frontend is an hour not spent
on the engine that is the actual point of this project. Plain TypeScript + canvas is sufficient and
signals better than a React app with a component library.
