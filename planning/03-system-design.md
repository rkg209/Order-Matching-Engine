# Velox Matching Engine — System Design

**Version:** 1.0 | **Status:** Implementation-Ready | **Codename:** `velox`

---

## Table of Contents

1. [Modules](#1-modules)
2. [Services](#2-services)
3. [Internal Workflows](#3-internal-workflows)
4. [Event Flows](#4-event-flows)
5. [State Transitions](#5-state-transitions)
6. [Design Patterns](#6-design-patterns)
7. [Integration Points](#7-integration-points)

---

## 1. Modules

Modules are the compilation and ownership units of the codebase. Each module owns a coherent slice of responsibility, has a defined allocation policy, and communicates with other modules only through the interfaces described here. The hot-path / off-path classification is the primary design constraint on every module.

---

### 1.1 `engine` — Matching Hot Path

**Allocation policy:** Zero allocations during steady-state operation. Enforced by a Google Benchmark allocation-counting profiler in CI.

**Thread affinity:** All classes in this module are touched exclusively by the `matching-engine` OS thread. No other thread may read or write any field of any object owned by this module during steady-state operation.

#### Classes

| Class | Role |
|---|---|
| `MatchingEngine` | Single-writer event loop. Consumes `CommandEvent` from the inbound SPSC ring, dispatches to handlers, emits results onto the outbound ring. |
| `OrderBook` | Per-instrument container. Holds `BidLevels`, `AskLevels`, `OrderIdMap`, and the `ObjectPool<Order>`. Exposes `processNewOrder`, `processCancel`, `processCancelReplace`. |
| `PriceLevel` | Intrusive doubly-linked FIFO queue of `Order` nodes at one price point. `enqueue` is O(1) tail insert; `remove` is O(1) pointer unlink; `head` is O(1) peek. |
| `Order` | Flyweight pooled object. Fields: `orderId` (int64_t), `side` (byte), `price` (int64_t), `quantity` (int64_t), `remainingQty` (int64_t), `participantId` (int64_t), `globalSeq` (int64_t), `prev`/`next` (Order — intrusive list pointers). |
| `Trade` | Flyweight pooled object. Fields: `tradeId` (int64_t), `aggressorOrderId` (int64_t), `passiveOrderId` (int64_t), `price` (int64_t), `quantity` (int64_t), `globalSeq` (int64_t). |
| `SelfTradePolicy` | Stateless strategy. Evaluates whether two matched orders share a `participantId` and applies the configured STP action (cancel aggressor, cancel passive, cancel both, allow). |

#### Internal dependencies

```
MatchingEngine
  └── OrderBook
        ├── BidLevels          (book module)
        ├── AskLevels          (book module)
        ├── OrderIdMap         (book module)
        ├── ObjectPool<Order>  (engine module)
        ├── ObjectPool<Trade>  (engine module)
        └── SelfTradePolicy    (engine module)
```

---

### 1.2 `book` — Order Book Data Structures

**Allocation policy:** Zero allocations during steady-state operation. Pre-allocated at startup.

**Thread affinity:** Owned exclusively by the `matching-engine` thread.

#### Classes

| Class | Role |
|---|---|
| `BidLevels` | Price-level index for the buy side. Backed by a custom `int64_t`-keyed open-addressing map to `PriceLevel*`, pre-sized at startup, keyed by price-as-int64_t (price × tick multiplier). Tracks `bestBidPrice` as a separate `int64_t` field, updated on every insert and remove. Iteration order is descending by price. |
| `AskLevels` | Price-level index for the sell side. Same structure as `BidLevels`; tracks `bestAskPrice`; iteration order is ascending by price. |
| `OrderIdMap` | Custom `int64_t`-keyed open-addressing map to `Order*` mapping `orderId → Order`. Enables O(1) cancel and cancel/replace without price-level traversal. |

#### Key invariants

- `BidLevels.bestBidPrice` is always the highest price with at least one resting order, or `std::numeric_limits<int64_t>::min()` when the bid side is empty.
- `AskLevels.bestAskPrice` is always the lowest price with at least one resting order, or `std::numeric_limits<int64_t>::max()` when the ask side is empty.
- A `PriceLevel` is removed from the map when its queue becomes empty, preventing stale empty levels from accumulating.
- Every `Order` in `BidLevels` or `AskLevels` has a corresponding entry in `OrderIdMap`. These two structures are always mutually consistent.

---

### 1.3 `sequencer` — Sequencing and Durability

**Allocation policy:** May allocate I/O buffers. Not on the hot path.

**Thread affinity:** `sequencer` OS thread.

#### Classes

| Class | Role |
|---|---|
| `Sequencer` | Receives decoded `CommandEvent` data from the gateway, assigns a strictly-increasing `globalSeqNum` via `globalSeq.fetch_add(1, std::memory_order_relaxed)`, calls `Journal.append()`, then publishes the event onto the inbound SPSC ring. |
| `Journal` | Append-only, segmented binary log. Each record layout: `[4-byte length][8-byte globalSeq][N-byte command payload]`. Segments roll at a configurable size (default 256 MB). Calls `fsync`/`fdatasync` (via `pwrite` on a raw file descriptor) after each record before the sequencer publishes to the ring. Maintains a `currentSegmentIndex` and a pre-sized write buffer. |
| `Snapshot` | Serializes the full engine state (all `OrderBook` instances, `globalSeqNum`) to a binary snapshot file on a configurable interval (default every 100,000 commands). Snapshot filename encodes the `globalSeqNum` at the time of the snapshot. Snapshot is written by a dedicated snapshot thread that receives a deep-copy of engine state; the matching engine is not paused. |

#### Journal record layout

```
Offset  Size  Field
------  ----  -----
0       4     recordLength (excludes this field)
4       8     globalSeqNum
12      1     commandType  (NEW_ORDER=1, CANCEL=2, CANCEL_REPLACE=3)
13      N     commandPayload (fixed-size per commandType, see protocol module)
```

---

### 1.4 `gateway` — TCP Order Gateway

**Allocation policy:** May allocate freely. Boost.Asio async I/O, off hot path.

**Thread affinity:** Boost.Asio `io_context` handling connections concurrently (`gateway-conn-{id}` per connection).

#### Classes

| Class | Role |
|---|---|
| `OrderGateway` | Opens a Boost.Asio `tcp::acceptor` on port 9001. Accept loop runs on a dedicated OS thread driving the `io_context`. Each accepted connection is handed to a new `ClientSession` async handler chain. |
| `ClientSession` | Owns one TCP connection. Reads bytes into a fixed buffer, calls `FrameDecoder.decode()`, calls `AuthHandler.check()`, assigns `clientSeqNum`, detects duplicate/gap client sequence numbers, and calls `RingBufferProducer.publish()`. Maintains a `sessionId` (int64_t) and a `participantId` (int64_t) assigned at authentication. |
| `FrameDecoder` | Stateless. Reads the 4-byte length prefix, validates total frame length, reads the message type byte, validates field ranges (price > 0, quantity > 0, side ∈ {BUY, SELL}), and populates a `CommandEvent` staging object. Returns a `DecodeResult` enum: `OK`, `INCOMPLETE`, `INVALID`. |
| `FrameEncoder` | Stateless. Serializes an `ExecutionReport` or `Reject` message into a fixed buffer for writing to the client socket. |
| `AuthHandler` | Validates the session token presented in the `LOGIN` frame against a pre-loaded credential store (in-memory `std::unordered_map<int64_t, std::array<uint8_t,32>>` of participantId → hashed token). Returns `AuthResult`: `ACCEPTED`, `REJECTED`. |

---

### 1.5 `protocol` — Wire Protocol Definitions

**Allocation policy:** Value objects; may be allocated by the gateway (off hot path). On the ring, flyweight `CommandEvent` objects are used instead.

#### Classes

| Class | Role |
|---|---|
| `MessageType` | Enum: `LOGIN`, `NEW_ORDER`, `CANCEL`, `CANCEL_REPLACE`, `EXEC_REPORT`, `REJECT`, `HEARTBEAT`. Each variant carries its fixed wire size in bytes. |
| `NewOrderMessage` | Fixed-layout binary frame definition. Fields: `clientSeqNum` (8), `orderId` (8), `instrumentId` (4), `side` (1), `orderType` (1), `price` (8), `quantity` (8), `timeInForce` (1). Total: 39 bytes payload. |
| `CancelMessage` | Fields: `clientSeqNum` (8), `orderId` (8), `instrumentId` (4). Total: 20 bytes payload. |
| `CancelReplaceMessage` | Fields: `clientSeqNum` (8), `orderId` (8), `instrumentId` (4), `newPrice` (8), `newQuantity` (8). Total: 36 bytes payload. |
| `ExecutionReport` | Fields: `orderId` (8), `execType` (1), `execQty` (8), `leavesQty` (8), `tradePrice` (8), `tradeId` (8), `globalSeq` (8). Total: 49 bytes payload. |
| `RejectMessage` | Fields: `orderId` (8), `rejectReason` (1), `globalSeq` (8). Total: 17 bytes payload. |

All prices are encoded as `long` (price × 10,000 to represent four decimal places without floating point). All quantities are `long` shares/contracts.

---

### 1.6 `ring` — Hand-Rolled SPSC Ring Buffer Configuration

**Allocation policy:** Pre-allocated at startup. Zero allocations during steady state.

#### Classes

| Class | Role |
|---|---|
| `CommandRingBuffer` | Configures and holds the inbound `SpscRing<CommandEvent>`. Single-producer, single-consumer, cache-line-padded head/tail indices. Ring size: 65,536 (configurable, must be power of two). Wait strategy: busy-spin (`_mm_pause()` in the spin loop) for the matching engine thread (lowest latency, burns a core). |
| `CommandEvent` | Flyweight event object pre-allocated on the ring. Fields: `commandType` (`uint8_t`), `orderId` (int64_t), `instrumentId` (`int32_t`), `side` (`uint8_t`), `orderType` (`uint8_t`), `price` (int64_t), `quantity` (int64_t), `participantId` (int64_t), `globalSeq` (int64_t), `timeInForce` (`uint8_t`). No pointers/references — all primitives (trivially copyable). |
| `OutboundRingBuffer` | Configures and holds the outbound `SpscRing<OutboundEvent>`. Single-producer (matching engine), two consumers in a dependent chain: `ExecutionReportRouter` and `MarketDataPublisher`. Ring size: 65,536. |
| `OutboundEvent` | Flyweight. Fields: `eventType` (`uint8_t`: EXEC_REPORT or MARKET_DATA), `orderId` (int64_t), `execType` (`uint8_t`), `execQty` (int64_t), `leavesQty` (int64_t), `tradePrice` (int64_t), `tradeId` (int64_t), `participantId` (int64_t), `side` (`uint8_t`), `instrumentId` (`int32_t`), `globalSeq` (int64_t). |
| `RingBufferProducer` | One instance per `ClientSession`. Wraps the ring's `try_publish()` call. On ring-full, returns `false` to `ClientSession`, which stops reading from the socket (backpressure). |

---

### 1.7 `marketdata` — Market-Data Publishing

**Allocation policy:** May allocate for serialization. Off hot path.

**Thread affinity:** `market-data-publisher` OS thread.

#### Classes

| Class | Role |
|---|---|
| `MarketDataPublisher` | Ring-buffer consumer for `OutboundEvent`. Receives every `OutboundEvent` from the outbound ring. Maintains a local mirror of the top-N price levels per instrument (configurable depth, default 10). On each event, computes the incremental L2 delta and constructs a `BookUpdateEvent` or `TradeTickEvent`. |
| `BookUpdateEvent` | Plain struct. Fields: `instrumentId`, `side`, `price`, `newQty`, `sequenceNum`. Serialized to JSON for WebSocket broadcast. |
| `TradeTickEvent` | Plain struct. Fields: `instrumentId`, `price`, `quantity`, `aggressorSide`, `tradeId`, `sequenceNum`. Serialized to JSON. |
| `WebSocketBroadcaster` | Holds a mutex-guarded `std::vector<std::shared_ptr<WebSocketSession>>` (Boost.Beast session handles) of active subscribers. `broadcast(std::string json)` iterates the list and issues an async write to each session (non-blocking). Manages subscriber registration and deregistration on connect/disconnect. |

---

### 1.8 `recovery` — Crash Recovery

**Allocation policy:** May allocate freely. Runs only at startup.

**Thread affinity:** Main thread during startup sequence.

#### Classes

| Class | Role |
|---|---|
| `JournalReplayer` | Opens journal segment files in order. Reads each record, validates the `globalSeqNum` is strictly sequential (no gaps, no duplicates). Constructs a `CommandEvent` from the record payload and calls `MatchingEngine.process(event)` directly, bypassing the gateway and ring. Stops at the target sequence number or end of journal. |
| `SnapshotLoader` | Reads the most recent valid snapshot file (highest `globalSeqNum` in filename with a valid checksum). Deserializes `OrderBook` state and `globalSeqNum` directly into the engine's data structures. Validates the snapshot checksum (CRC32) before applying. |

---

### 1.9 `telemetry` — Observability

**Allocation policy:** Off hot path. `LatencyRecorder.recordValue()` is a single `int64_t` write with no allocation; all other telemetry operations may allocate.

#### Classes

| Class | Role |
|---|---|
| `LatencyRecorder` | Wraps an HdrHistogram_c `hdr_histogram` configured for nanosecond resolution (1 ns to 10 s range). The matching engine calls `hdr_record_value(int64_t elapsedNs)` at the end of each order processing cycle. The histogram is accessed by the matching engine thread for writes and by `LatencyPublisher` for reads; coordination uses HdrHistogram_c's thread-safe recorder wrapper to avoid contention. |
| `Counters` | Holds one `alignas(64) std::atomic<int64_t>` per counter, each padded to 64 bytes to prevent false sharing. Counters: `ordersReceived`, `ordersMatched`, `ordersCancelled`, `ordersRejected`, `tradesExecuted`, `ringUtilizationSamples`. The matching engine increments these with no locking (`fetch_add(1, std::memory_order_relaxed)`). |
| `LatencyPublisher` | Runs on a dedicated OS thread. Wakes once per second, calls `LatencyRecorder.getIntervalHistogram()` (resets the interval), serializes `{ p50, p99, p999, max, count }` to JSON, and calls `WebSocketBroadcaster.broadcastLatency(json)`. |

---

### 1.10 `benchmark` — Performance Measurement

**Allocation policy:** Unrestricted. Benchmark harness only.

#### Classes

| Class | Role |
|---|---|
| `MatchingEngineBenchmark` | Google Benchmark `BENCHMARK()` fixture. Uses `VELOX_MODE=bench` startup mode. Drives `MatchingEngine.process()` directly with pre-constructed `CommandEvent` objects. Measures throughput (ops/sec) and latency (ns/op) with `Unit(benchmark::kMicrosecond)` and multiple iterations. |
| `LoadGenerator` | End-to-end load driver. Opens TCP connections to port 9001, sends `NewOrderMessage` frames at a configurable rate (default 1,000,000 orders/sec), reads and discards `ExecutionReport` responses. Records round-trip latency using HdrHistogram_c. |
| `BenchmarkHarness` | Wires Google Benchmark with HdrHistogram_c output. Writes results to `benchmarks/baselines/` as CSV and HDR files. Reads hardware metadata from `benchmarks/baselines/hardware.md` and embeds it in the result file. |

---

### 1.11 `visualizer` — Live Visualization

**Allocation policy:** Unrestricted. Off hot path, separate concern.

#### Sub-modules

| Sub-module | Role |
|---|---|
| `visualizer/server` | Boost.Beast HTTP/WebSocket handler. Handles WebSocket upgrade requests on port 8080. Serves static frontend files from a known directory on disk. Registers new `WebSocketSession` instances with `WebSocketBroadcaster` on connect. |
| `visualizer/frontend` | TypeScript + Canvas web application. Connects to `ws://localhost:8080/stream`. Maintains a local order-book mirror (sorted `Map<price, qty>` for bids and asks). Applies incremental `L2` updates. Renders the book ladder at 60 fps via `requestAnimationFrame`. Renders the latency histogram as a bar chart updated on each `LATENCY` message. |

---

## 2. Services

Services are the runtime execution units — threads or thread pools — that bring the modules to life. Each service has a defined lifecycle, a thread model, and a clear set of responsibilities.

---

### 2.1 `MatchingEngineService`

| Property | Value |
|---|---|
| Thread type | OS thread, pinned |
| Thread name | `matching-engine` |
| CPU affinity | Core N (configured via `VELOX_MATCHING_CPU`, applied with `pthread_setaffinity_np`/`sched_setaffinity`) |
| Priority | `SCHED_FIFO` real-time priority where permitted, else normal scheduling |
| Lifecycle | Started after recovery completes; runs until process shutdown |

**Responsibilities:**
- Spin on the inbound `CommandRingBuffer` using a busy-spin wait loop (`_mm_pause()`).
- For each available `CommandEvent`, dispatch to `OrderBook.processNewOrder`, `processCancel`, or `processCancelReplace`.
- Write results to the outbound `OutboundRingBuffer`.
- Call `LatencyRecorder.recordValue()` and `Counters.ordersMatched.fetch_add(1, std::memory_order_relaxed)` after each event.
- Never block, never allocate, never perform I/O.

**Startup sequence:**
1. Receive signal from `RecoveryService` that recovery is complete.
2. Acquire CPU affinity.
3. Enter the ring-buffer event loop.

**Shutdown sequence:**
1. Receive shutdown signal (POSIX `SIGTERM`/`SIGINT` handler).
2. Drain remaining events from the ring.
3. Signal `SnapshotService` to write a final snapshot.
4. Exit event loop.

---

### 2.2 `SequencerService`

| Property | Value |
|---|---|
| Thread type | OS thread |
| Thread name | `sequencer` |
| Lifecycle | Started after recovery; stopped before `MatchingEngineService` |

**Responsibilities:**
- Receive decoded commands from `ClientSession` async handlers via a hand-off queue (a second, smaller SPSC/MPSC ring or a mutex+condvar-guarded bounded queue of `CommandEvent` — the sequencer is the single consumer of this staging queue).
- Assign `globalSeqNum` via `globalSeq.fetch_add(1, std::memory_order_relaxed)`.
- Call `Journal.append(globalSeqNum, commandBytes)` and wait for fsync.
- Publish the `CommandEvent` onto the inbound `CommandRingBuffer`.

**Backpressure:** If the inbound `CommandRingBuffer` is full, `RingBufferProducer.try_publish()` returns false. The `SequencerService` spins briefly, then applies backpressure upstream to the staging queue, which in turn causes `ClientSession` handlers to block on the queue put, which stops them reading from their sockets, propagating TCP flow control to the client.

---

### 2.3 `GatewayService`

| Property | Value |
|---|---|
| Thread type | One OS thread driving the accept loop's `io_context`; connections multiplexed via Boost.Asio async handlers (optionally a small `io_context` thread pool) |
| Thread names | `gateway-accept`, `gateway-conn-{id}` |
| Lifecycle | Started last (after engine is ready); stopped first on shutdown |

**Responsibilities:**
- `gateway-accept` thread: runs the Boost.Asio `tcp::acceptor` async-accept loop, creates a `ClientSession`, hands it off to the `io_context`.
- Each `gateway-conn-{id}` async handler chain: reads frames, decodes, authenticates, publishes to the sequencer staging queue.
- On connection close or error: deregisters the `ClientSession` from the `ExecutionReportRouter`'s session map.

---

### 2.4 `ExecutionReportRouterService`

| Property | Value |
|---|---|
| Thread type | OS thread |
| Thread name | `exec-report-router` |
| Lifecycle | Started with the engine; stopped after the engine drains |

**Responsibilities:**
- Consume `OutboundEvent` objects of type `EXEC_REPORT` from the outbound `OutboundRingBuffer`.
- Look up the `ClientSession` by `participantId` from a custom `int64_t`-keyed open-addressing map to `ClientSession*` maintained by the router.
- Call `FrameEncoder.encode(event)` to produce a fixed buffer.
- Write the buffer to the client socket via `ClientSession.writeResponse(buffer)`.
- Handle write errors (broken pipe) by marking the session as disconnected.

---

### 2.5 `MarketDataService`

| Property | Value |
|---|---|
| Thread type | OS thread |
| Thread name | `market-data-publisher` |
| Lifecycle | Started with the engine |

**Responsibilities:**
- Consume all `OutboundEvent` objects from the outbound ring (dependent consumer, runs after `ExecutionReportRouterService` in the ring-buffer consumer chain).
- Maintain the local book-depth mirror.
- Construct and broadcast `BookUpdateEvent` and `TradeTickEvent` JSON messages via `WebSocketBroadcaster`.

---

### 2.6 `SnapshotService`

| Property | Value |
|---|---|
| Thread type | OS thread |
| Thread name | `snapshot-writer` |
| Lifecycle | Runs continuously; triggered by counter threshold or shutdown signal |

**Responsibilities:**
- Monitor `Counters.ordersMatched`. When the count since the last snapshot exceeds `VELOX_SNAPSHOT_INTERVAL` (default 100,000), request a snapshot.
- Snapshot request mechanism: the matching engine, at the end of a processing cycle, checks a `std::atomic<bool> snapshotRequested` flag. When true, it serializes its state into a pre-allocated buffer (no allocation — the buffer is pre-sized at startup) and signals the `SnapshotService` via a `std::condition_variable`.
- `SnapshotService` receives the buffer, writes it to `snapshot-{globalSeq}.bin`, computes and appends a CRC32 checksum, and renames the file atomically.
- Old snapshots beyond the retention count (default 3) are deleted.

---

### 2.7 `RecoveryService`

| Property | Value |
|---|---|
| Thread type | Main thread (startup only) |
| Lifecycle | Runs once at startup; terminates before normal operation begins |

**Responsibilities:**
- Determine startup mode from `VELOX_MODE`.
- In `live` and `replay` modes: call `SnapshotLoader.load()`, then `JournalReplayer.replay()`.
- In `bench` mode: skip recovery entirely.
- Signal `MatchingEngineService` to start when recovery is complete.

---

### 2.8 `LatencyPublisherService`

| Property | Value |
|---|---|
| Thread type | OS thread |
| Thread name | `latency-publisher` |
| Lifecycle | Runs continuously; 1 Hz wake cycle |

**Responsibilities:**
- Wake once per second via `std::this_thread::sleep_for(std::chrono::seconds(1))`.
- Call `LatencyRecorder.getIntervalHistogram()` to get and reset the interval histogram.
- Serialize to JSON: `{ type:"LATENCY", p50, p99, p999, max, count, timestamp }`.
- Call `WebSocketBroadcaster.broadcastLatency(json)`.
- Read `Counters` fields and include in the telemetry payload.

---

### 2.9 `WebSocketServerService`

| Property | Value |
|---|---|
| Thread type | Boost.Beast/Asio thread pool (default 8 OS threads) |
| Thread names | `ws-server-{n}` |
| Lifecycle | Started at process start; independent of engine state |

**Responsibilities:**
- Serve the static visualizer frontend files from a known directory at `GET /`.
- Handle WebSocket upgrade at `GET /stream`.
- Register/deregister `WebSocketSession` instances with `WebSocketBroadcaster`.
- The WebSocket endpoint is read-only: the server ignores any messages sent by the client.

---

## 3. Internal Workflows

Internal workflows describe the step-by-step execution paths through the system for each significant operation. Each step names the exact class and method involved.

---

### 3.1 Workflow: Process New Limit Order

This is the primary hot-path workflow. Every step after the ring-buffer handoff executes on the `matching-engine` thread with zero allocations.

```
Step  Thread                  Class.Method                          Notes
----  ------                  ------------                          -----
1     gateway-conn-{id}       FrameDecoder.decode(buffer)           Validates frame; populates staging CommandEvent
2     gateway-conn-{id}       AuthHandler.check(ClientSession)      Validates session token
3     gateway-conn-{id}       RingBufferProducer.publish(event)     Writes to sequencer staging queue
4     sequencer               Sequencer.onCommand(event)            Assigns globalSeqNum
5     sequencer               Journal.append(seq, payload)          Writes record; calls fsync/fdatasync
6     sequencer               CommandRingBuffer.publish(event)      Claims ring slot; writes fields; commits
7     matching-engine         MatchingEngine.onEvent(event, seq, eob)  Ring-buffer callback
8     matching-engine         MatchingEngine.dispatchNewOrder(event)
9     matching-engine         OrderBook.processNewOrder(order)
10    matching-engine         [matching loop — see 3.1.1 below]
11    matching-engine         PriceLevel.enqueue(order)             If residual qty > 0 and LIMIT
12    matching-engine         OrderIdMap.put(orderId, order)        Register for future cancel
13    matching-engine         OutboundRingBuffer.publish(execReport) Emit EXEC_REPORT(NEW_ACK or FILL)
14    matching-engine         LatencyRecorder.recordValue(elapsed)
15    exec-report-router      ExecutionReportRouterService.onEvent() Encode and write to client socket
16    market-data-publisher   MarketDataPublisher.onEvent()         Compute L2 delta; broadcast
```

#### 3.1.1 Matching Loop Detail (Step 10)

```
while true:
  bestOppositePrice = (order.side == BUY) ? askLevels.bestAskPrice
                                           : bidLevels.bestBidPrice
  if order.side == BUY  and bestOppositePrice > order.price: break
  if order.side == SELL and bestOppositePrice < order.price: break
  if bestOppositePrice == INT64_MAX or INT64_MIN:  break   // empty book

  level = (order.side == BUY) ? askLevels.get(bestOppositePrice)
                               : bidLevels.get(bestOppositePrice)
  passive = level.head()

  // Self-trade check
  if SelfTradePolicy.evaluate(order, passive) == CANCEL_AGGRESSOR:
    emit ExecReport(order.id, CANCELLED, STP)
    break
  if SelfTradePolicy.evaluate(order, passive) == CANCEL_PASSIVE:
    level.remove(passive)
    orderIdMap.remove(passive.orderId)
    orderPool.release(passive)
    emit ExecReport(passive.id, CANCELLED, STP)
    continue

  fillQty = min(order.remainingQty, passive.remainingQty)
  tradePrice = passive.price                                // passive price wins
  tradeId = tradeIdSequence.fetch_add(1, std::memory_order_relaxed)

  emit Trade(tradeId, order.id, passive.id, tradePrice, fillQty)
  emit ExecReport(order.id,    PARTIAL_FILL or FILL, fillQty, tradePrice, tradeId)
  emit ExecReport(passive.id,  PARTIAL_FILL or FILL, fillQty, tradePrice, tradeId)

  passive.remainingQty -= fillQty
  if passive.remainingQty == 0:
    level.remove(passive)                                   // O(1) intrusive unlink
    orderIdMap.remove(passive.orderId)
    orderPool.release(passive)
    if level.isEmpty(): askLevels.removeLevel(bestOppositePrice)  // update bestAsk

  order.remainingQty -= fillQty
  if order.remainingQty == 0: break

// Post-loop
if order.remainingQty > 0:
  if order.timeInForce == LIMIT:
    level = bidLevels.getOrCreate(order.price)
    level.enqueue(order)
    orderIdMap.put(order.orderId, order)
    bidLevels.updateBestBid(order.price)
  else if order.timeInForce == IOC or MARKET:
    emit ExecReport(order.id, CANCELLED, RESIDUAL)
    orderPool.release(order)
```

---

### 3.2 Workflow: Process Cancel Order

```
Step  Thread           Class.Method                              Notes
----  ------           ------------                              -----
1-6   (same as 3.1, steps 1-6, with CancelMessage payload)
7     matching-engine  MatchingEngine.dispatchCancel(event)
8     matching-engine  order = OrderIdMap.get(event.orderId)
9a    [not found]      emit OutboundEvent(EXEC_REPORT, REJECT, UNKNOWN_ORDER)   → step 13
9b    [found]          PriceLevel.remove(order)                  O(1) intrusive unlink
10    matching-engine  OrderIdMap.remove(event.orderId)
11    matching-engine  orderPool.release(order)
12    matching-engine  emit OutboundEvent(EXEC_REPORT, CANCELLED)
13    matching-engine  OutboundRingBuffer.publish(event)
14    exec-report-router  encode and write to client socket
15    market-data-publisher  compute L2 delta (qty at price level decreased); broadcast
```

---

### 3.3 Workflow: Process Cancel/Replace Order

Cancel/replace is implemented as an atomic cancel-then-new-order. Time priority resets because the order receives a new position in the queue.

```
Step  Thread           Class.Method
----  ------           ------------
1-6   (same as 3.1, steps 1-6, with CancelReplaceMessage payload)
7     matching-engine  MatchingEngine.