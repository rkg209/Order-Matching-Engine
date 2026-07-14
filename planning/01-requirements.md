# requirements.md — Velox Matching Engine

---

## 1. Business Goals

| ID | Goal |
|----|------|
| BG-1 | Demonstrate a production-credible, low-latency order-matching engine that serves as the primary portfolio showpiece for performance-engineering, real-time systems, and mechanical-sympathy skills. |
| BG-2 | Produce *measured*, reproducible latency numbers (p50/p99/p999) that can be cited verbatim on a résumé and defended in technical interviews at low-latency, quant, and HFT-adjacent firms. |
| BG-3 | Prove correctness through automated, deterministic replay and property-based testing — not by assertion alone. |
| BG-4 | Demonstrate systems-thinking breadth beyond a pure data-structure exercise: binary wire protocol, event-sourced journaling, crash recovery, market-data distribution, and a live visualizer. |
| BG-5 | Demonstrate a zero-allocation, lock-free hot-path design measured directly in C++ — no garbage collector to reason about, because there isn't one. |
| BG-6 | Establish a spec-driven development workflow (Spec Kit + Claude Code) as a reusable, committed portfolio artifact that reviewers can inspect alongside the code. |

---

## 2. Stakeholders

| ID | Stakeholder | Interest |
|----|-------------|----------|
| SH-1 | **Project author / engineer** | Builds and owns the system; primary user of every component; the résumé and interview-defense beneficiary. |
| SH-2 | **Technical interviewers** (low-latency / quant / HFT / infra roles) | Evaluate architecture decisions, latency methodology, and correctness proof; read the spec backlog and design writeup as portfolio artifacts. |
| SH-3 | **Non-technical / recruiting reviewers** | Assess the live visualizer demo and the headline numbers in the README; need a tangible, self-explanatory artifact. |
| SH-4 | **Future contributors / open-source readers** | Consume the committed `.claude/` guardrails, spec backlog, and design writeup as reference material. |

---

## 3. Users / Personas

| ID | Persona | Description | Primary Touchpoints |
|----|---------|-------------|---------------------|
| UP-1 | **Order gateway client** | A simulated or real trading client that submits orders and receives execution reports over the binary TCP protocol. | Binary order gateway (Spec 007). |
| UP-2 | **Market-data subscriber** | A process or the live visualizer that consumes the incremental L2/L3 book-update and trade-tick stream. | Market-data publisher (Spec 008), WebSocket feed (Spec 010). |
| UP-3 | **Benchmark operator** | The engineer running `/bench`, `/replay`, `/invariants`, and `/alloc-check` to validate and gate changes. | Google Benchmark harness, HdrHistogram output, CI pipeline. |
| UP-4 | **Visualizer viewer** | A human (recruiter or interviewer) watching the animated order-book ladder and real-time latency histogram in a browser. | Live web visualizer (Spec 010). |
| UP-5 | **Recovery operator** | The engineer or automated test that kills the engine mid-stream and restarts it to verify byte-identical state recovery. | Journal, snapshot, replay subsystem (Spec 006). |

---

## 4. Functional Requirements

### 4.1 Order Book & Matching Core

| ID | Requirement |
|----|-------------|
| FR-1 | The system SHALL maintain an in-memory order book for each active instrument, with bids sorted descending by price and asks sorted ascending by price. |
| FR-2 | Each price level SHALL maintain a FIFO queue of resting orders, implemented as an intrusive doubly-linked list, so that time priority within a price level is preserved exactly. |
| FR-3 | The system SHALL maintain an `orderId → order-node` map enabling O(1) lookup for cancel and cancel/replace operations without traversing price levels. |
| FR-4 | The matching engine SHALL match incoming orders against the opposite side of the book by **price-time priority**: best price first, then earliest-submitted order at that price. |
| FR-5 | The matching engine SHALL support **limit orders**: an order to buy or sell at a specified price or better; unmatched residual quantity SHALL rest at its price level. |
| FR-6 | The matching engine SHALL support **market orders**: an order to buy or sell at the best available price; the order SHALL match against resting orders until fully filled or the book is exhausted. |
| FR-7 | The matching engine SHALL support **Immediate-Or-Cancel (IOC) orders**: match as much as possible immediately; any unfilled residual SHALL be cancelled without resting. |
| FR-8 | The matching engine SHALL support **Fill-Or-Kill (FOK) orders**: execute the full quantity immediately or cancel the entire order; no partial resting is permitted. |
| FR-9 | The matching engine SHALL support **cancel orders**: remove a resting order identified by `orderId`; if the order does not exist or is already filled, return an appropriate rejection. |
| FR-10 | The matching engine SHALL support **cancel/replace (modify) orders**: atomically cancel a resting order and submit a replacement with new price and/or quantity; time priority is reset on replace. |
| FR-11 | The matching engine SHALL correctly handle **partial fills**: when a resting order is partially matched, its remaining quantity SHALL stay at its price level in its original queue position. |
| FR-12 | The matching engine SHALL handle **crossing books**: if a new resting limit order would immediately cross the spread (its price is at or better than the best opposite price), it SHALL match rather than rest. |
| FR-13 | The matching engine SHALL implement **self-trade prevention**: if the aggressor and the resting order share the same participant identifier, the match SHALL be suppressed and the aggressor cancelled (or rejected, per configured STP mode). |
| FR-14 | The matching engine SHALL emit an **execution report** for every fill (full or partial), cancel, replace, and rejection, containing: `orderId`, `side`, `price`, `filledQty`, `remainingQty`, `tradeId` (for fills), and `globalSequenceNumber`. |
| FR-15 | The matching engine SHALL emit a **trade tick** for every matched pair, containing: `tradeId`, `aggressor orderId`, `passive orderId`, `price`, `quantity`, `timestamp (logical sequence)`. |

### 4.2 Sequencer & Journal (Event Sourcing)

| ID | Requirement |
|----|-------------|
| FR-16 | The sequencer SHALL assign a **global monotonic sequence number** to every inbound command before it is processed by the matching engine; this sequence number is the authoritative ordering of all events. |
| FR-17 | The sequencer SHALL **append every inbound command** (with its sequence number) to a durable, segmented journal file before the command is dispatched to the matching engine. |
| FR-18 | Journal segments SHALL be flushed to durable storage (`fsync`) before the corresponding command is acknowledged as sequenced. |
| FR-19 | The system SHALL support **periodic snapshots** of the full engine state (order book + sequence counter) to bound recovery replay time. |
| FR-20 | On restart, the system SHALL **replay the journal** from the most recent valid snapshot, reconstructing engine state deterministically; the recovered state SHALL be byte-identical to the pre-crash state at the same sequence number. |
| FR-21 | Replaying the same journal SHALL always produce the **same sequence of trades and execution reports** — no non-determinism may be introduced by wall-clock time, random numbers, or hash-map iteration order. |

### 4.3 Order Gateway (Binary Wire Protocol)

| ID | Requirement |
|----|-------------|
| FR-22 | The order gateway SHALL accept TCP connections from clients and communicate using a **length-prefixed binary protocol** with fixed-format frames for: `NewOrder`, `CancelOrder`, `CancelReplaceOrder`, and `ExecutionReport`. |
| FR-23 | The gateway SHALL **decode and validate** each inbound frame: reject frames with invalid length, unknown message type, out-of-range field values, or missing required fields, and return a structured rejection message. |
| FR-24 | The gateway SHALL perform **authentication** of connecting clients before accepting order messages; unauthenticated connections SHALL be closed. |
| FR-25 | The gateway SHALL assign a **client sequence number** to each inbound order and detect gaps (duplicate or out-of-order client sequences), rejecting duplicates and flagging gaps. |
| FR-26 | The gateway SHALL publish validated, decoded commands onto the inbound ring buffer as a **single producer** per connection, without blocking the matching thread. |
| FR-27 | The gateway SHALL route **execution reports** from the matching engine back to the originating client connection. |
| FR-28 | The gateway SHALL handle **backpressure**: if the inbound ring buffer is full, the gateway SHALL apply flow control to the client (e.g., stop reading) rather than dropping orders silently. |
| FR-29 | The gateway SHALL survive a **fuzz pass** (malformed, truncated, and hostile input) without crashing or corrupting engine state. |
| FR-30 | Each client connection SHALL be handled by **Boost.Asio async I/O (or a dedicated thread-per-connection)**, isolated from the matching hot path. |

### 4.4 Market-Data Publisher

| ID | Requirement |
|----|-------------|
| FR-31 | The market-data publisher SHALL emit **incremental L2 order-book updates** after every matching cycle: price-level additions, modifications, and deletions on both sides. |
| FR-32 | The market-data publisher SHALL emit **L3 per-order updates** (new order, cancel, fill) sufficient for a subscriber to reconstruct the full order-by-order book. |
| FR-33 | The market-data publisher SHALL emit **trade ticks** for every matched trade (see FR-15). |
| FR-34 | A subscriber consuming only the market-data feed from a clean start SHALL be able to **reconstruct the order book identically** to the engine's internal state at any point in time. |
| FR-35 | The market-data publisher SHALL operate as an **off-hot-path consumer** of the outbound ring buffer; it SHALL NOT block or slow the matching engine. |

### 4.5 Live Visualizer

| ID | Requirement |
|----|-------------|
| FR-36 | The live visualizer SHALL be a **read-only web application** that connects to the market-data + latency stream over WebSocket. |
| FR-37 | The visualizer SHALL render an **animated order-book ladder** showing the top N bid and ask price levels with their aggregate quantities, updating in real time as the book changes. |
| FR-38 | The visualizer SHALL render a **real-time latency histogram** (p50, p99, p999 bars or distribution curve) sourced from the HdrHistogram output stream. |
| FR-39 | The visualizer SHALL be **clearly decoupled from the hot path**: it SHALL consume only the published market-data and latency streams and SHALL NOT interact with the matching engine directly. |
| FR-40 | The visualizer SHALL function correctly when fed a **replayed session** (pre-recorded journal) as well as a live session, enabling a deterministic demo. |

### 4.6 Benchmarking & Measurement

| ID | Requirement |
|----|-------------|
| FR-41 | The system SHALL include a **Google Benchmark microbenchmark suite** measuring order-to-match latency (p50, p99, p999) and throughput (orders/sec) for the matching hot path in isolation. |
| FR-42 | The system SHALL include a **load generator** that drives the full end-to-end path (gateway → ring → engine → execution report) at configurable rates. |
| FR-43 | All latency measurements SHALL use **HdrHistogram_c** with coordinated-omission correction enabled; averages alone are insufficient and SHALL NOT be the headline metric. |
| FR-44 | Benchmark results SHALL be persisted as a **committed baseline JSON file** (`benchmarks/baselines/summary.json`) containing p50, p99, p999, and throughput on stated hardware. |
| FR-45 | The `/bench` command SHALL **diff current results against the committed baseline** and report any p99 regression exceeding the budget as a hard failure. |
| FR-46 | The system SHALL generate a **latency distribution plot** (histogram image) as part of every benchmark run. |

### 4.7 Correctness Testing

| ID | Requirement |
|----|-------------|
| FR-47 | The system SHALL include a **golden replay test suite**: a set of named scenarios (covering every order type and edge case) where a fixed input journal is replayed and the output trades and execution reports are compared byte-for-byte against a reference file. |
| FR-48 | Golden replay scenarios SHALL cover at minimum: limit order match, partial fill, full fill, cancel, cancel/replace, market order, IOC, FOK, crossing book, self-trade prevention, and empty-book market order. |
| FR-49 | The system SHALL include a **property-based test suite** that asserts the following invariants hold after every operation across thousands of randomized order schedules: (a) quantity conservation — total quantity in the book plus filled quantity equals total submitted quantity; (b) sequence monotonicity — global sequence numbers are strictly increasing; (c) no crossed book — after every matching cycle, the best bid price is strictly less than the best ask price (or the book is one-sided); (d) FIFO fairness — within a price level, orders are filled in submission order. |
| FR-50 | The system SHALL include a **recovery test**: the engine is started, a journal is written, the process is killed mid-stream, the engine is restarted, the journal is replayed, and the recovered state is asserted byte-identical to the pre-kill state. |

### 4.8 Multi-Instrument Support (Optional — Spec 011)

| ID | Requirement |
|----|-------------|
| FR-51 | The system SHOULD support **multiple instruments**, each with its own isolated order book and matching thread, such that a failure or slowdown in one instrument does not affect others. |
| FR-52 | Per-instrument **determinism** SHALL be preserved independently; aggregate throughput SHALL scale with the number of instruments. |

---

## 5. Non-Functional Requirements

### 5.1 Latency

| ID | Requirement |
|----|-------------|
| NFR-1 | The matching hot path SHALL achieve a **median (p50) order-to-match latency of ≤ 2 µs** under sustained load on the target development hardware, as measured by Google Benchmark with HdrHistogram_c. |
| NFR-2 | The matching hot path SHALL achieve a **p99 order-to-match latency of ≤ 20 µs** under sustained load on the target development hardware. |
| NFR-3 | The matching hot path SHALL achieve a **p999 order-to-match latency of ≤ 100 µs** under sustained load on the target development hardware. |
| NFR-4 | All latency targets (NFR-1 through NFR-3) SHALL be measured **without coordinated omission** (using HdrHistogram_c's interval histogram or equivalent corrected recording). |
| NFR-5 | Latency measurements SHALL be taken on **pinned, stated hardware** with the hardware configuration documented alongside the baseline numbers; results SHALL NOT be presented without the hardware context. |
| NFR-6 | A **p99 regression of more than 20%** above the committed baseline SHALL be treated as a hard build failure, blocking merge. |

### 5.2 Throughput

| ID | Requirement |
|----|-------------|
| NFR-7 | The matching engine SHALL sustain a throughput of **≥ 1,000,000 orders/sec** on the single matching thread under benchmark conditions. |
| NFR-8 | Throughput SHALL be measured end-to-end through the ring buffer (not just the book data structure in isolation) and reported alongside latency numbers. |

### 5.3 Memory & Allocation

| ID | Requirement |
|----|-------------|
| NFR-9 | The matching hot path (all code executed per order in `engine/` and `book/` packages during steady-state processing) SHALL allocate **zero bytes per operation** as measured by the allocation profiler. |
| NFR-10 | Object pools and flyweight event objects SHALL be used for all order and trade representations on the hot path; pool exhaustion SHALL be handled gracefully (backpressure, not allocation). |
| NFR-11 | All containers on the hot path SHALL use **custom pool-allocated or open-addressing structures**, pre-sized at startup; standard containers that reallocate (`std::vector::push_back` without reservation, `std::unordered_map` without a fixed bucket count) are forbidden on the hot path. |
| NFR-12 | **Dynamic memory allocation** (`new`, `malloc`, container reallocation) of any kind is forbidden on the hot path. |

### 5.4 Concurrency & Thread Safety

| ID | Requirement |
|----|-------------|
| NFR-13 | The matching engine SHALL execute on a **single, dedicated OS thread** (pinned where the OS permits); no other thread SHALL read or write engine state during steady-state operation. |
| NFR-14 | Cross-thread handoff between the gateway (producer) and the matching engine (consumer) SHALL occur **exclusively via the hand-rolled lock-free SPSC ring buffer**; no `std::mutex`, `std::lock_guard`, or other blocking primitives are permitted on the hot path. |
| NFR-15 | The market-data publisher and execution-report router SHALL operate as **off-hot-path ring-buffer consumers**; they SHALL NOT block the matching engine thread. |
| NFR-16 | **False sharing** between hot-path data structures and adjacent fields SHALL be eliminated via cache-line padding (`alignas(64)` or manual padding). |

### 5.5 Determinism

| ID | Requirement |
|----|-------------|
| NFR-17 | Given the same input journal, the matching engine SHALL produce **byte-identical output** on every replay, regardless of wall-clock time, compiler/standard-library version (within the pinned C++20 toolchain), or run count. |
| NFR-18 | The engine SHALL NOT use `std::chrono::system_clock`, wall-clock reads, `std::rand`/`<random>`, UUID generation, or any other source of non-determinism on the hot path (only `std::chrono::steady_clock` is permitted, for latency measurement). |
| NFR-19 | Hash maps used on the hot path SHALL use **deterministically-ordered** implementations; a hash map whose iteration order depends on non-deterministic hashing (e.g., randomized seed) is forbidden on the hot path. |

### 5.6 Correctness

| ID | Requirement |
|----|-------------|
| NFR-20 | Every order type and edge case enumerated in FR-48 SHALL have a corresponding golden replay scenario; the replay test suite SHALL be a **mandatory CI gate**. |
| NFR-21 | The four book invariants defined in FR-49 SHALL hold after **every single operation** in the property-based test suite; a single invariant violation is a hard failure. |
| NFR-22 | No order SHALL be **lost, duplicated, or double-filled** under any tested schedule, including concurrent cancel and fill of the same order. |

### 5.7 Durability & Recovery

| ID | Requirement |
|----|-------------|
| NFR-23 | The journal SHALL be **durable before acknowledgement**: a command is not considered sequenced until its journal write has been flushed to storage (`fsync` or equivalent). |
| NFR-24 | Recovery from a crash at any point in the journal SHALL produce a state that is **byte-identical** to the pre-crash state at the same sequence number, with no manual intervention. |
| NFR-25 | Snapshot + replay recovery time SHALL be bounded; the system SHALL be configurable with a **maximum replay distance** (commands since last snapshot) to cap restart latency. |

### 5.8 Robustness & Security

| ID | Requirement |
|----|-------------|
| NFR-26 | The order gateway SHALL reject **all malformed, truncated, and hostile input** without crashing, corrupting engine state, or leaking internal state to the client. |
| NFR-27 | A fuzz pass (automated, covering at minimum 10,000 randomized malformed frames) SHALL be part of the gateway test suite and SHALL find **zero crashes or state corruptions**. |
| NFR-28 | Unauthenticated connections SHALL be **closed within a configurable timeout** without consuming matching-engine resources. |

### 5.9 Observability

| ID | Requirement |
|----|-------------|
| NFR-29 | The system SHALL expose **off-hot-path counters** for: orders received, orders matched, orders cancelled, orders rejected, trades executed, ring-buffer utilization, and journal write latency. |
| NFR-30 | **No logging call** (spdlog, iostream, printf, etc.) SHALL appear on the hot path; telemetry is counter-based and consumed off-thread. |
| NFR-31 | The live visualizer SHALL display the **current p50/p99/p999 latency** sourced from a streaming HdrHistogram snapshot, updated at least once per second. |

### 5.10 Testability & CI

| ID | Requirement |
|----|-------------|
| NFR-32 | The following CMake/CTest targets SHALL exist and be independently executable: `test` (unit + property), `replay_test` (golden scenarios), `invariant_test` (property-based), `bench` (Google Benchmark), `alloc_check` (allocation check). |
| NFR-33 | CI SHALL run `test`, `replay_test`, and `invariant_test` on every pull request; `bench` and `alloc_check` SHALL run on every merge to `main`. |
| NFR-34 | A **p99 regression gate** (NFR-6) SHALL be enforced in CI by the `/bench` command comparing against `benchmarks/baselines/summary.json`. |

### 5.11 Code Quality & Maintainability

| ID | Requirement |
|----|-------------|
| NFR-35 | All C++ source SHALL be formatted by **clang-format** on every write; unformatted code SHALL be rejected by CI. |
| NFR-36 | Hot-path modules (`engine/`, `book/`) SHALL be subject to the **`latency-reviewer` sub-agent scan** after every edit, catching forbidden patterns (allocation, locking, exceptions, virtual dispatch, logging) before merge. |
| NFR-37 | Every new behavioral capability SHALL be accompanied by either a golden replay scenario or a property test before the implementing task is considered done. |

---

## 6. Constraints

| ID | Constraint |
|----|------------|
| CON-1 | **Primary and only language is C++20.** The matching engine, gateway, sequencer, market-data publisher, and test harness are all implemented in C++20. There is no second implementation in another language. |
| CON-2 | **Single-threaded matching hot path.** The architectural decision to use a single matching thread is locked and SHALL NOT be re-litigated. Scaling is achieved by sharding per instrument (FR-51), not by parallelizing the matching thread. |
| CON-3 | **A hand-rolled lock-free SPSC ring buffer is the mandatory ring-buffer implementation.** No alternative concurrent queue (e.g., a mutex-guarded `std::deque`) is permitted for the hot-path handoff. |
| CON-4 | **Custom pool/arena allocators and open-addressing containers are mandatory** for hot-path data structures. |
| CON-5 | **HdrHistogram_c is the mandatory latency-measurement library.** Averages or simple timing loops are not acceptable as the headline latency metric. |
| CON-6 | **Google Benchmark is the mandatory microbenchmark framework.** Ad-hoc timing loops are not acceptable for published benchmark results. |
| CON-7 | **The visualizer is strictly read-only.** It SHALL NOT send any message to the matching engine, gateway, or sequencer. It consumes only the published market-data and latency streams. |
| CON-8 | **No external database or persistent store** is used for the engine's primary state. The system is in-memory; the journal is the sole durability mechanism. |
| CON-9 | **No cloud or managed services** are required to run the system. The full stack (engine, gateway, visualizer) SHALL run on a single developer machine. |
| CON-10 | **Latency numbers SHALL always be reported with hardware context.** A number without stated hardware is not a valid result. |
| CON-11 | **The spec backlog (`specs/`) is a committed portfolio artifact.** Specs SHALL NOT be deleted after implementation; they remain in the repository as evidence of spec-driven development. |
| CON-12 | **The `.claude/` directory (commands, agents, skills, hooks, settings) SHALL be committed** to the repository so guardrails travel with the code. |

---

## 7. Technologies

### 7.1 Mandated (explicitly specified)

| Technology | Version | Role |
|------------|---------|------|
| C++ | 20 | Primary and only implementation language; Boost.Asio async I/O for gateway; dedicated pinned OS thread for matching engine. |
| Hand-rolled SPSC ring buffer | — | Lock-free ring buffer for hot-path ingress; single-writer handoff between gateway/sequencer and matching engine. |
| Custom pool/arena allocators | — | Pooled and arena-allocated data structures, open-addressing hash maps; mandatory for hot-path data structures. |
| HdrHistogram_c | Latest stable | Coordinated-omission-free latency capture; mandatory for all published latency measurements. |
| Google Benchmark | Latest stable | Microbenchmark framework; mandatory for all published throughput and latency benchmarks. |
| perf / Valgrind (callgrind) | Latest stable | Flamegraph and allocation profiling of the hot path. |
| GoogleTest | Latest stable | Unit and integration test framework. |
| Property-test helpers | — | Lightweight property-based testing helpers for invariant and randomized schedule tests. |
| CMake | Latest stable | Build system; hosts all target definitions (`test`, `replay_test`, `invariant_test`, `bench`, `alloc_check`). |
| clang-format / clang-tidy | Latest stable | Code formatter and static analyzer; enforced on every write via CI. |
| Boost.Beast / Boost.Asio | Latest stable | REST management API + WebSocket market-data/visualizer server. |
| Docker | Latest stable | Containerization for reproducible build and demo environments. |
| Spec Kit (`specify-cli`) | Latest | SDD backbone; persists constitution, specs, plans, and tasks to Git. |

### 7.2 Implied / Recommended

| Technology | Role |
|------------|------|
| TypeScript + Canvas (or React) | Live visualizer frontend; consumes WebSocket market-data and latency stream. |
| WebSocket | Transport between market-data publisher and live visualizer. |
| GitHub Actions | CI pipeline executing test, replay, invariant, benchmark, and allocation-check gates. |
| GitHub MCP server | Maps spec backlog tasks to tracked issues; enables PR review against the constitution. |
| Playwright / Puppeteer (optional) | End-to-end testing of the visualizer UI only; never touches the engine. |

---

## 8. Deployment Requirements

| ID | Requirement |
|----|-------------|
| DR-1 | The full system (matching engine, order gateway, sequencer/journal, market-data publisher, and live visualizer) SHALL be runnable on a **single developer machine** as a native binary with no external runtime dependencies beyond a browser. |
| DR-2 | A **Docker Compose configuration** SHALL be provided that starts all components (engine, gateway, market-data publisher, visualizer server) with a single command (`docker compose up`). |
| DR-3 | The benchmark suite (`ctest -L bench` / `./build/bench/velox_bench`) SHALL be runnable in isolation without starting the gateway or visualizer, so latency measurements are not contaminated by unrelated I/O. |
| DR-4 | The journal directory SHALL be **configurable via an environment variable or configuration file**; the default SHALL be a local directory that persists across container restarts when mounted as a volume. |
| DR-5 | The system SHALL support a **replay-only mode**: start the engine, replay a specified journal file to a specified sequence number, and halt — used for recovery testing and golden-replay CI. |
| DR-6 | The live visualizer SHALL be accessible via a **standard web browser** (Chrome, Firefox, Safari — latest stable versions) at a configurable port (default: `8080`) with no browser plugin required. |
| DR-7 | All **benchmark baseline files** (`benchmarks/baselines/`) SHALL be committed to the repository and SHALL NOT be modified except by the explicit `/perf-baseline` command, which requires a deliberate operator action. |
| DR-8 | The system SHALL document the **exact hardware configuration** (CPU model, core count, RAM, OS, compiler version, launch flags) used to produce the committed baseline numbers, in `benchmarks/baselines/hardware.md`. |
| DR-9 | CI (GitHub Actions) SHALL execute the following gates on every pull request: `ctest -L unit`, `ctest -L replay`, `ctest -L invariant`; and on every merge to `main`: additionally `ctest -L bench` and `ctest -L alloc_check`, with the p99 regression gate (NFR-6) enforced. |
| DR-10 | The system SHALL be buildable via a **single top-level CMake configuration** for the whole project (engine, gateway, market-data, visualizer server, and test/benchmark targets). |

---

*Every functional requirement (FR-1 through FR-52) and non-functional requirement (NFR-1 through NFR-37) is uniquely numbered for reference by downstream feature specs, plans, and tasks. Deployment requirements (DR-1 through DR-10) and constraints (CON-1 through CON-12) are similarly numbered.*