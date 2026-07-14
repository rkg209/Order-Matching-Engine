# Velox Matching Engine — Architecture Document

**Version:** 1.0 | **Status:** Implementation-Ready | **Codename:** `velox`

---

## Table of Contents

1. [System Overview](#1-system-overview)
2. [Component Architecture](#2-component-architecture)
3. [Data Flow](#3-data-flow)
4. [Service Boundaries](#4-service-boundaries)
5. [Deployment Architecture](#5-deployment-architecture)
6. [Scaling Strategy](#6-scaling-strategy)
7. [Security Architecture](#7-security-architecture)
8. [Technology Stack](#8-technology-stack)

---

## 1. System Overview

Velox is a single-process, in-memory order-matching engine that implements the core of a financial exchange. It accepts buy and sell orders from clients over a binary TCP protocol, matches them by price-time priority, and distributes execution reports and market-data updates to consumers — all with a latency-disciplined hot path designed for microsecond-range tail latency.

### 1.1 Architectural Philosophy

The system is organized around one inviolable principle: **the matching hot path is sacred**. Every architectural decision is evaluated first by whether it protects the hot path from latency, allocation, or non-determinism. Components that cannot meet that standard are placed off the hot path and communicate with it exclusively through the hand-rolled lock-free SPSC ring buffer.

Three properties are simultaneously maintained and mutually reinforcing:

- **Determinism:** the same input journal always produces byte-identical output. This enables crash recovery and reproducible correctness tests — both from one mechanism.
- **Zero-allocation hot path:** no object creation during steady-state order processing. Enforced by tooling, not convention.
- **Single-writer matching thread:** one thread owns all engine state. No locks, no contention, no coordinated omission from lock-induced pauses.

### 1.2 System Boundaries

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            VELOX PROCESS                                    │
│                                                                             │
│  ┌──────────────┐    ┌─────────────┐    ┌──────────────────────────────┐   │
│  │ Order Gateway│    │  Sequencer  │    │     Matching Engine          │   │
│  │  (I/O layer) │───▶│  + Journal  │───▶│  (single-threaded hot path)  │   │
│  └──────────────┘    └─────────────┘    └──────────────────────────────┘   │
│          ▲                                          │                       │
│          │                               ┌──────────┴──────────┐           │
│          │                               ▼                     ▼           │
│          │                    ┌──────────────────┐  ┌──────────────────┐   │
│          └────────────────────│ Execution Report │  │  Market-Data     │   │
│                               │    Router        │  │  Publisher       │   │
│                               └──────────────────┘  └──────────────────┘   │
│                                                              │              │
│                                                    ┌─────────▼──────────┐  │
│                                                    │  WebSocket Server  │  │
│                                                    └────────────────────┘  │
└─────────────────────────────────────────────────────────────────────────────┘
         ▲                                                      │
         │ TCP (binary)                                         │ WebSocket
    ┌────┴────┐                                         ┌───────▼───────┐
    │ Clients │                                         │  Visualizer   │
    └─────────┘                                         │  (browser)    │
                                                        └───────────────┘
```

**What is inside the process:** Order Gateway, Sequencer, Journal, Matching Engine, Execution Report Router, Market-Data Publisher, WebSocket Server.

**What is outside the process:** Trading clients (TCP), the live visualizer (browser/WebSocket), and the journal files on disk.

---

## 2. Component Architecture

### 2.1 Component Map

```
velox-matching-engine/
├── engine/                       ← HOT PATH (zero-allocation zone), namespace velox::engine
│   ├── matching_engine.hpp/.cpp  ← single-writer event loop
│   ├── order_book.hpp/.cpp       ← per-instrument book
│   ├── price_level.hpp/.cpp      ← intrusive FIFO queue at one price
│   ├── order.hpp                 ← flyweight / pooled
│   ├── trade.hpp                 ← flyweight / pooled
│   └── self_trade_policy.hpp/.cpp ← STP logic
├── book/                         ← HOT PATH (data structures), namespace velox::book
│   ├── order_id_map.hpp/.cpp     ← orderId → Order node, O(1) lookup
│   ├── bid_levels.hpp/.cpp       ← price-level index, bids DESC
│   └── ask_levels.hpp/.cpp       ← price-level index, asks ASC
├── sequencer/                    ← namespace velox::sequencer
│   ├── sequencer.hpp/.cpp        ← assigns global monotonic seq#
│   ├── journal.hpp/.cpp          ← append-only, segmented, fsync
│   └── snapshot.hpp/.cpp         ← periodic state serialization
├── gateway/                      ← namespace velox::gateway
│   ├── order_gateway.hpp/.cpp    ← TCP accept loop (Boost.Asio)
│   ├── client_session.hpp/.cpp   ← per-connection handler
│   ├── frame_decoder.hpp/.cpp    ← binary protocol decode + validate
│   ├── frame_encoder.hpp/.cpp    ← binary protocol encode
│   └── auth_handler.hpp/.cpp     ← connection authentication
├── protocol/                     ← namespace velox::protocol
│   ├── message_type.hpp          ← enum: NEW_ORDER, CANCEL, REPLACE, EXEC_REPORT, REJECT
│   ├── new_order_message.hpp     ← fixed-layout binary frame
│   ├── cancel_message.hpp
│   ├── cancel_replace_message.hpp
│   └── execution_report.hpp
├── marketdata/                   ← namespace velox::marketdata
│   ├── market_data_publisher.hpp/.cpp  ← ring consumer, off hot path
│   ├── book_update_event.hpp     ← L2/L3 incremental update
│   ├── trade_tick_event.hpp
│   └── websocket_broadcaster.hpp/.cpp  ← fans out to visualizer subscribers
├── ring/                         ← namespace velox::ring
│   ├── command_ring_buffer.hpp   ← hand-rolled SPSC ring configuration
│   ├── command_event.hpp         ← flyweight event on the ring
│   └── ring_buffer_producer.hpp/.cpp   ← gateway → ring (single producer per connection)
├── recovery/                     ← namespace velox::recovery
│   ├── journal_replayer.hpp/.cpp ← reads journal, feeds engine deterministically
│   └── snapshot_loader.hpp/.cpp  ← deserializes snapshot to engine state
├── telemetry/                    ← namespace velox::telemetry
│   ├── latency_recorder.hpp/.cpp ← HdrHistogram_c, off hot path
│   ├── counters.hpp              ← lock-free padded atomic counters
│   └── latency_publisher.hpp/.cpp ← streams histogram snapshots to visualizer
├── benchmark/                    ← namespace velox::benchmark
│   ├── matching_engine_benchmark.cpp  ← Google Benchmark entry point
│   ├── load_generator.hpp/.cpp        ← end-to-end load driver
│   └── benchmark_harness.hpp/.cpp     ← wires Google Benchmark + HdrHistogram_c
├── visualizer/                   ← off hot path, separate process or thread
│   ├── server/                   ← WebSocket + HTTP server (Boost.Beast)
│   └── frontend/                 ← TypeScript + Canvas web app
└── CMakeLists.txt                ← top-level build; each module has its own CMakeLists.txt
```

### 2.2 Component Descriptions

#### 2.2.1 Matching Engine (Hot Path)

The single most important component. Runs on one dedicated OS thread, pinned to a CPU core where the OS permits. Owns all order book state exclusively — no other thread reads or writes it during steady-state operation.

**Responsibilities:**
- Consume `CommandEvent` objects from the hand-rolled SPSC ring buffer one at a time.
- Dispatch to the appropriate handler: `processNewOrder`, `processCancel`, `processCancelReplace`.
- Execute price-time priority matching against the order book.
- Emit `ExecutionReport` and `Trade` events onto the outbound ring.
- Maintain zero allocations per operation during steady state.

**What it does NOT do:** I/O, logging, clock reads, random number generation, or any blocking operation.

#### 2.2.2 Order Book

One `OrderBook` instance per instrument. Contains:

- **`BidLevels`:** price-level index for the buy side, ordered descending by price. Implemented as a custom open-addressing `int64_t`-keyed map to `PriceLevel*` keyed by price-as-integer (price × tick-size multiplier to avoid floating point), pre-sized at startup. The best bid is tracked as a separate field updated on every insert/remove.
- **`AskLevels`:** price-level index for the sell side, ordered ascending by price. Same structure.
- **`OrderIdMap`:** custom open-addressing `int64_t`-keyed map to `Order*` mapping `orderId` to the `Order` node. Enables O(1) cancel and cancel/replace without traversing price levels.

**`PriceLevel`:** an intrusive doubly-linked FIFO queue of `Order` nodes at one price. "Intrusive" means the `Order` object itself carries `prev` and `next` pointers — no wrapper node allocation. Insert is O(1) at the tail. Remove (cancel) is O(1) given the node reference from `OrderIdMap`. Match walks from the head.

**`Order` (flyweight/pooled):** obtained from an `ObjectPool<Order>` template (RAII acquire/release) pre-allocated at startup. Fields: `orderId` (`int64_t`), `side` (`uint8_t`), `price` (`int64_t`), `quantity` (`int64_t`), `remainingQty` (`int64_t`), `participantId` (`int64_t`), `globalSeq` (`int64_t`), `prev`/`next` (`Order*` for the intrusive list). Returned to the pool on full fill or cancel.

#### 2.2.3 Sequencer + Journal

Runs on its own thread, upstream of the matching engine. Receives decoded commands from the gateway, assigns a strictly-increasing global sequence number, appends the command + sequence number to the journal, and then publishes the command onto the SPSC ring for the matching engine.

**Journal format:** append-only, segmented files. Each record: `[4-byte length][8-byte globalSeq][N-byte command payload]`. Segments roll at a configurable size (default 256 MB). A POSIX `fsync`/`fdatasync` is called after each write (via `pwrite` on a raw file descriptor) before the command is published to the ring. This is the durability guarantee.

**Snapshot:** periodically (configurable interval, default every 100,000 commands), the engine serializes its full state (all order books, sequence counter) to a snapshot file. Recovery replays only from the most recent valid snapshot, bounding replay time.

#### 2.2.4 Order Gateway

Accepts TCP connections. Each connection is handled via Boost.Asio async I/O (an `io_context`-driven accept loop and per-connection coroutine/handler chain) — cheap, non-blocking I/O is fine here because this is off the hot path. The connection handler reads frames, decodes them via `FrameDecoder`, validates fields, authenticates the session, assigns a client sequence number, and publishes the decoded command onto the SPSC ring as a single producer.

The gateway is the only component that allocates freely — it is not on the hot path. Execution reports flow back from the matching engine via the outbound ring to the `ExecutionReportRouter`, which looks up the originating `ClientSession` and writes the encoded response.

#### 2.2.5 Hand-Rolled SPSC Ring Buffer

The sole cross-thread communication mechanism between the gateway/sequencer and the matching engine. Configured as a single-producer, single-consumer ring (one sequencer thread → one matching engine thread), with cache-line-padded head/tail indices and a busy-spin wait strategy. `CommandEvent` objects on the ring are pre-allocated flyweights; the producer writes field values into the existing object, never allocates a new one.

Ring size: power of two, default 65,536 slots. Backpressure: if the ring is full, the gateway stops reading from the client socket (TCP flow control propagates back to the sender).

#### 2.2.6 Market-Data Publisher

A ring-buffer consumer that runs after the matching engine in the same pipeline (dependent consumer chain). Receives the same events the matching engine processed, plus the trade and execution-report events the engine emitted. Constructs incremental L2 and L3 book-update messages and trade ticks, and hands them to the `WebSocketBroadcaster`. Runs off the hot path — it does not block the matching engine.

#### 2.2.7 Telemetry

`LatencyRecorder` wraps an HdrHistogram_c histogram and is updated by the matching engine at the end of each order processing cycle (recording the elapsed nanoseconds from ring-buffer claim to match completion). The histogram is read by `LatencyPublisher` on a separate thread (once per second) and streamed to the visualizer. The matching engine writes to the histogram using `hdr_record_value` — a single integer write, no allocation.

`Counters` uses `alignas(64) std::atomic<int64_t>` fields (one per counter, padded to avoid false sharing) for orders received, matched, cancelled, rejected, trades executed, and ring utilization. Read by the telemetry publisher off-thread.

#### 2.2.8 Live Visualizer

A read-only web application. The backend is a lightweight HTTP/WebSocket server (Boost.Beast, running in its own thread pool, entirely off the hot path). The frontend is TypeScript + Canvas, served as static files. It connects to the WebSocket endpoint, receives JSON-encoded book-update and latency-snapshot messages, and renders the order-book ladder and latency histogram. It never sends any message to the engine.

---

## 3. Data Flow

### 3.1 Inbound Order Flow (Happy Path)

```
Client (TCP)
    │
    │  [binary frame: NewOrder]
    │  4-byte length prefix + fixed-layout payload
    ▼
ClientSession (Boost.Asio async handler)
    │  FrameDecoder.decode() → validates length, message type, field ranges
    │  AuthHandler.check()   → validates session token
    │  assigns clientSeqNum, detects duplicates/gaps
    ▼
Sequencer (sequencer thread)
    │  assigns globalSeqNum (globalSeq.fetch_add(1, std::memory_order_relaxed))
    │  Journal.append(globalSeqNum, commandBytes)  ← fsync before continuing
    │  CommandRingBuffer.publish(commandEvent)      ← single-producer claim
    ▼
SPSC Ring Buffer
    │  [CommandEvent flyweight: type, orderId, side, price, qty, participantId, globalSeq]
    ▼
MatchingEngine (matching thread — single writer)
    │  Order node = orderPool.acquire()
    │  populate Order fields from CommandEvent
    │  OrderBook.processNewOrder(order):
    │    while book.bestOpposite().price crosses order.price:
    │      passive = bestOpposite().head()
    │      fillQty = min(order.remainingQty, passive.remainingQty)
    │      emit Trade(tradeId, order.id, passive.id, price, fillQty)
    │      emit ExecReport(order.id, PARTIAL_FILL/FILL, fillQty, ...)
    │      emit ExecReport(passive.id, PARTIAL_FILL/FILL, fillQty, ...)
    │      passive.remainingQty -= fillQty
    │      if passive fully filled: dequeue from PriceLevel, return to pool
    │      order.remainingQty -= fillQty
    │      if order fully filled: break
    │    if order.remainingQty > 0 and order.type == LIMIT:
    │      PriceLevel.enqueue(order)   ← O(1) tail insert
    │      OrderIdMap.put(order.id, order)
    │    else if IOC/market: cancel residual
    │  LatencyRecorder.recordValue(elapsed)
    ▼
Outbound SPSC Ring (matching thread → consumers)
    ├──▶ ExecutionReportRouter (consumer thread)
    │        looks up ClientSession by participantId
    │        FrameEncoder.encode(execReport) → writes to socket
    │
    └──▶ MarketDataPublisher (consumer thread)
             constructs L2/L3 incremental update
             constructs TradeTick
             WebSocketBroadcaster.broadcast(message)
                  └──▶ Visualizer (browser, WebSocket)
```

### 3.2 Cancel Flow

```
Client → [CancelOrder: orderId]
    │
    ▼ (gateway decode + sequencer journal — same as above)
    │
MatchingEngine:
    │  order = OrderIdMap.get(orderId)          ← O(1)
    │  if order == null: emit Reject(UNKNOWN_ORDER)
    │  else:
    │    PriceLevel.remove(order)               ← O(1) intrusive list unlink
    │    OrderIdMap.remove(orderId)             ← O(1)
    │    orderPool.release(order)
    │    emit ExecReport(orderId, CANCELLED, ...)
```

### 3.3 Cancel/Replace Flow

```
MatchingEngine:
    │  old = OrderIdMap.get(orderId)            ← O(1)
    │  if old == null: emit Reject(UNKNOWN_ORDER)
    │  else:
    │    PriceLevel.remove(old)                 ← O(1) unlink
    │    OrderIdMap.remove(old.id)
    │    orderPool.release(old)
    │    emit ExecReport(old.id, CANCELLED, ...)
    │    // now process as new order (time priority resets)
    │    newOrder = orderPool.acquire()
    │    populate from CancelReplace fields
    │    OrderBook.processNewOrder(newOrder)     ← may match immediately
```

### 3.4 Recovery / Replay Flow

```
Startup (recovery mode)
    │
    ▼
SnapshotLoader.load(latestSnapshot)
    │  deserializes OrderBook state + globalSeqNum into engine
    ▼
JournalReplayer.replay(journal, fromSeq=snapshot.seq+1)
    │  reads each journal record in order
    │  feeds CommandEvent directly into MatchingEngine.process()
    │  (bypasses gateway, sequencer, ring — deterministic single-thread replay)
    │  asserts output trades match expected (in recovery-test mode)
    ▼
Engine reaches pre-crash state at the last journaled sequence number
    │
    ▼
Normal operation resumes (gateway opens, ring starts)
```

### 3.5 Market-Data Subscription Flow

```
Subscriber connects via WebSocket to port 8080
    │
    ▼
WebSocketBroadcaster registers subscriber
    │
    ▼
MarketDataPublisher (running continuously as ring-buffer consumer):
    │  on each BookUpdateEvent:
    │    serialize to JSON: { type:"L2", side:"BID", price:..., qty:..., seq:... }
    │    WebSocketBroadcaster.broadcast(json)
    │  on each TradeTickEvent:
    │    serialize to JSON: { type:"TRADE", price:..., qty:..., seq:... }
    │    WebSocketBroadcaster.broadcast(json)
    │
    ▼
Visualizer frontend:
    │  receives JSON messages over WebSocket
    │  applies incremental updates to local book mirror
    │  renders order-book ladder (Canvas, 60fps animation frame)
    │  renders latency histogram (updated 1/sec from LatencyPublisher stream)
```

### 3.6 Latency Measurement Flow

```
MatchingEngine (matching thread):
    │  startNs = std::chrono::steady_clock::now()   ← taken at ring-buffer event claim
    │  ... process order ...
    │  elapsed = std::chrono::steady_clock::now() - startNs
    │  latencyHistogram.recordValue(elapsed)   ← single long write, no alloc

LatencyPublisher (separate thread, 1Hz):
    │  snapshot = latencyHistogram.copy()
    │  reset interval histogram
    │  serialize: { p50:..., p99:..., p999:..., max:... }
    │  WebSocketBroadcaster.broadcastLatency(snapshot)
    │
    ▼
Visualizer: updates histogram bars in real time
```

---

## 4. Service Boundaries

### 4.1 Boundary Definitions

All components run in a single native process. The boundaries are **thread boundaries and ring-buffer handoffs**, not network boundaries. This is the correct architecture for a latency-obsessed system — network hops between components would add milliseconds, not microseconds.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  BOUNDARY 1: TCP network (clients → gateway)                                │
│  Protocol: length-prefixed binary frames over TCP                           │
│  Ownership: gateway owns the socket; clients own nothing inside the process │
├─────────────────────────────────────────────────────────────────────────────┤
│  BOUNDARY 2: SPSC ring buffer (gateway/sequencer → matching engine)         │
│  Protocol: CommandEvent flyweight objects on a pre-allocated ring            │
│  Ownership: sequencer is sole producer; matching engine is sole consumer    │
│  Guarantee: single-producer, single-consumer; no locks; cache-line padded   │
├─────────────────────────────────────────────────────────────────────────────┤
│  BOUNDARY 3: Outbound SPSC ring buffer (matching engine → consumers)        │
│  Protocol: ExecutionReportEvent + MarketDataEvent flyweights                │
│  Ownership: matching engine is sole producer; router + publisher consume    │
│  Guarantee: dependent consumer chain; publisher never blocks engine         │
├─────────────────────────────────────────────────────────────────────────────┤
│  BOUNDARY 4: Filesystem (sequencer → journal files)                         │
│  Protocol: binary append-only records, fsync per record                     │
│  Ownership: sequencer thread is sole writer; replayer reads at startup only │
├─────────────────────────────────────────────────────────────────────────────┤
│  BOUNDARY 5: WebSocket (market-data publisher → visualizer)                 │
│  Protocol: JSON messages over WebSocket                                     │
│  Ownership: broadcaster is sole writer; visualizer is read-only consumer    │
└─────────────────────────────────────────────────────────────────────────────┘
```

### 4.2 Thread Ownership Map

| Thread | Name | What it owns | May allocate? |
|--------|------|--------------|---------------|
| OS thread (pinned) | `matching-engine` | All `OrderBook`, `PriceLevel`, `OrderIdMap`, `Order` pool state | NO — zero alloc |
| OS thread | `sequencer` | Journal file handle, global sequence counter | Yes (I/O buffers) |
| Boost.Asio `io_context` (N connections) | `gateway-conn-{id}` | One `ClientSession` each | Yes |
| OS thread | `exec-report-router` | Outbound socket write buffers | Yes |
| OS thread | `market-data-publisher` | `BookUpdateEvent` serialization | Yes |
| OS thread | `latency-publisher` | HdrHistogram_c snapshot | Yes |
| Thread pool (Boost.Beast) | `ws-server-{n}` | WebSocket connections | Yes |

### 4.3 What Crosses Each Boundary

**Boundary 1 (TCP):** Raw bytes only. The gateway decodes them; the engine never sees raw bytes.

**Boundary 2 (inbound ring):** `CommandEvent` fields written by the sequencer, read by the matching engine. The `CommandEvent` object itself never crosses — it lives on the ring permanently. Only field values are "transferred."

**Boundary 3 (outbound ring):** `ExecutionReportEvent` and `MarketDataEvent` flyweights. Same pattern — objects are pre-allocated on the ring; field values are written by the engine and read by consumers.

**Boundary 4 (filesystem):** Serialized binary command records. The journal is the source of truth for recovery; the engine's in-memory state is a derived view.

**Boundary 5 (WebSocket):** JSON strings. The visualizer is a pure consumer; it sends nothing back through this boundary.

---

## 5. Deployment Architecture

### 5.1 Single-Machine Deployment (Primary)

The entire system runs as one native process on a single machine. This is the correct deployment for a latency-obsessed system and the only deployment mode for the benchmark numbers.

```
┌─────────────────────────────────────────────────────────────────┐
│  Developer Machine / Benchmark Host                             │
│                                                                 │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │  Native process: velox-engine                              │  │
│  │                                                           │  │
│  │  Threads:                                                 │  │
│  │    matching-engine (pinned OS thread, CPU core N)          │  │
│  │    sequencer       (OS thread)                              │  │
│  │    exec-report-router (OS thread)                           │  │
│  │    market-data-publisher (OS thread)                        │  │
│  │    latency-publisher (OS thread)                            │  │
│  │    gateway-conn-* (Boost.Asio io_context, N per client)    │  │
│  │    ws-server-*    (Boost.Beast thread pool)                 │  │
│  │                                                           │  │
│  │  Ports:                                                   │  │
│  │    9001/tcp  ← binary order gateway                       │  │
│  │    8080/tcp  ← WebSocket + static visualizer files        │  │
│  │                                                           │  │
│  │  Filesystem:                                              │  │
│  │    $VELOX_JOURNAL_DIR/journal-{seg}.bin                   │  │
│  │    $VELOX_JOURNAL_DIR/snapshot-{seq}.bin                  │  │
│  └───────────────────────────────────────────────────────────┘  │
│                                                                 │
│  ┌──────────────────────┐   ┌──────────────────────────────┐   │
│  │  Load Generator      │   │  Browser (Visualizer)        │   │
│  │  (separate native proc) │ localhost:8080                │   │
│  └──────────────────────┘   └──────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Docker Compose Deployment

For reproducible demo and CI environments. The engine runs in one container (built via a multi-stage Dockerfile: a build stage compiling with CMake, and a minimal runtime stage — e.g. distroless or slim Debian — shipping just the compiled binary and its shared-library dependencies); the load generator in another; the browser accesses the visualizer via a mapped port.

```yaml
# docker-compose.yml (illustrative structure)
services:
  velox-engine:
    image: velox-engine:latest
    ports:
      - "9001:9001"   # binary gateway
      - "8080:8080"   # visualizer + WebSocket
    volumes:
      - velox-journal:/var/velox/journal
    environment:
      - VELOX_JOURNAL_DIR=/var/velox/journal
      - VELOX_MATCHING_CPU=2          # pin matching thread to core 2
      - VELOX_RING_SIZE=65536
      - VELOX_SNAPSHOT_INTERVAL=100000
    ulimits:
      rtprio: 99                      # allow real-time thread priority

  velox-loadgen:
    image: velox-loadgen:latest
    depends_on: [velox-engine]
    environment:
      - TARGET_HOST=velox-engine
      - TARGET_PORT=9001
      - RATE=1000000                  # orders/sec

volumes:
  velox-journal:
```

### 5.3 Native Process Launch Configuration

These settings are committed to `scripts/run-engine.sh` and documented in `benchmarks/baselines/hardware.md`. They are not optional for the benchmark numbers. There is no garbage collector to tune — the launch configuration is entirely about CPU/NUMA placement and page pre-touch, which is a simplification relative to a managed runtime, not a gap.

```bash
# CPU pinning: isolate the matching thread on a dedicated core (also set in-process
# via pthread_setaffinity_np/sched_setaffinity as a defense-in-depth measure)
numactl --cpunodebind=0 --membind=0 \
  taskset -c 2 \
  ./velox-engine \
    --journal-dir "$VELOX_JOURNAL_DIR" \
    --matching-cpu 2 \
    --ring-size 65536 \
    --snapshot-interval 100000 \
    --prefault-pages \                   # mlockall(MCL_CURRENT|MCL_FUTURE) + madvise(MADV_WILLNEED) at startup
    --huge-pages                          # allocate hot-path arenas via mmap(MAP_HUGETLB) where available
```

### 5.4 Benchmark Isolation Requirements

Documented in `benchmarks/baselines/hardware.md` alongside every committed baseline:

- Matching thread pinned to an isolated CPU core (via `taskset` on Linux or `numactl`).
- CPU frequency scaling disabled (`cpupower frequency-set --governor performance`).
- Turbo boost disabled for reproducibility.
- IRQ affinity moved away from the matching core.
- No other significant processes on the benchmark host.
- Launch configuration as above, committed to `scripts/run-bench.sh`.

### 5.5 Startup Modes

The engine supports three startup modes, selected by the `VELOX_MODE` environment variable:

| Mode | Value | Behavior |
|------|-------|----------|
| **Normal** | `live` | Load latest snapshot, replay journal tail, open gateway, begin matching. |
| **Replay-only** | `replay` | Load snapshot, replay journal to specified `VELOX_REPLAY_TO_SEQ`, halt. Used for recovery tests and golden-replay CI. |
| **Benchmark** | `bench` | Skip gateway and journal; drive matching engine directly from the load generator via in-process call. Used for Google Benchmark microbenchmarks. |

---

## 6. Scaling Strategy

### 6.1 Primary Scaling Axis: Per-Instrument Sharding

The single-threaded matching model scales by **sharding per instrument**, not by parallelizing the matching thread. Each instrument gets its own matching thread, its own SPSC ring, and its own order book. Instruments are completely isolated — a slow instrument cannot affect a fast one.

```
Instrument Router (gateway thread)
    │
    ├──▶ Ring[AAPL] ──▶ MatchingEngine[AAPL] (thread, core 2)
    ├──▶ Ring[MSFT] ──▶ MatchingEngine[MSFT] (thread, core 3)
    ├──▶ Ring[TSLA] ──▶ MatchingEngine[TSLA] (thread, core 4)
    └──▶ Ring[BTC]  ──▶ MatchingEngine[BTC]  (thread, core 5)
```

The gateway decodes the instrument symbol from the inbound frame and routes to the correct ring. Each ring is a separate SPSC ring buffer instance. Each matching engine thread is pinned to a dedicated core.

**Aggregate throughput** scales linearly with the number of instruments (up to the number of