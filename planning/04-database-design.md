# Velox Matching Engine — Database Design

**Version:** 1.0 | **Status:** Implementation-Ready | **Codename:** `velox`

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Storage Tiers](#2-storage-tiers)
3. [Entity Catalog](#3-entity-catalog)
4. [Relational Schema](#4-relational-schema)
5. [Entity Relationships](#5-entity-relationships)
6. [Data Ownership by Module](#6-data-ownership-by-module)
7. [Multi-Tenancy Model](#7-multi-tenancy-model)
8. [Journal and Snapshot Formats](#8-journal-and-snapshot-formats)
9. [Schema Invariants and Constraints](#9-schema-invariants-and-constraints)
10. [Indexing Strategy](#10-indexing-strategy)
11. [Retention and Archival](#11-retention-and-archival)
12. [Design Rationale](#12-design-rationale)

---

## 1. Design Philosophy

Velox has **two completely separate storage concerns** that must never be conflated:

**The hot path has no database.** The matching engine's order book state lives exclusively in pre-allocated, pooled C++ structs / arena-allocated objects in process memory. No RDBMS, no embedded key-value store, no memory-mapped file is touched during order processing. The moment a database call appears on the hot path, the latency guarantees collapse.

**Everything else is durable.** The system's durability guarantee comes from the append-only journal written by the `sequencer` module. All other persistent state — participant credentials, instrument definitions, audit records, trade history — is stored in a relational database that is accessed exclusively by off-hot-path components.

This document covers both concerns: the relational schema for off-path persistent state, and the binary formats for the journal and snapshot files that constitute the engine's own durability layer.

The relational schema is designed for **PostgreSQL 16** as the reference implementation. It is accessed only by the gateway (authentication), the audit writer (post-trade), and administrative tooling. It is never accessed by the matching engine thread.

---

## 2. Storage Tiers

```
┌─────────────────────────────────────────────────────────────────────────────┐
│  TIER 1 — In-Process Memory (hot path)                                      │
│                                                                             │
│  Owner:   matching-engine thread exclusively                                │
│  What:    OrderBook, PriceLevel, Order pool, Trade pool, OrderIdMap         │
│  Format:  Pre-allocated C++ structs / arena-allocated objects, intrusive    │
│           linked lists                                                     │
│  Latency: Nanoseconds                                                       │
│  Durable: NO — reconstructed from Tier 2 on restart                        │
│  Access:  Single-writer, zero contention, zero I/O                          │
└─────────────────────────────────────────────────────────────────────────────┘
         │ serialized periodically + on shutdown
         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  TIER 2 — Filesystem (journal + snapshots)                                  │
│                                                                             │
│  Owner:   sequencer thread (journal writes); snapshot-writer thread         │
│  What:    Append-only command journal; periodic engine state snapshots      │
│  Format:  Custom binary (see §8)                                            │
│  Latency: Microseconds (fsync per record)                                   │
│  Durable: YES — primary durability guarantee                                │
│  Access:  Single writer; read-only at startup by RecoveryService            │
└─────────────────────────────────────────────────────────────────────────────┘
         │ post-trade audit writer reads outbound ring
         ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│  TIER 3 — Relational Database (PostgreSQL)                                  │
│                                                                             │
│  Owner:   Off-path services: gateway, audit writer, admin tooling           │
│  What:    Participants, instruments, sessions, trades, exec reports,        │
│           risk limits, audit log                                            │
│  Format:  Normalized relational schema (see §4)                             │
│  Latency: Milliseconds — acceptable, never on hot path                      │
│  Durable: YES — WAL-backed PostgreSQL                                       │
│  Access:  Multi-reader, serializable isolation for writes                   │
└─────────────────────────────────────────────────────────────────────────────┘
```

The three tiers are strictly ordered by latency requirement. Data flows **downward** (from Tier 1 toward Tier 3) as it ages and as latency requirements relax. Data flows **upward** only at startup (recovery from Tier 2) and at authentication (credential lookup from Tier 3 into the gateway's in-memory cache).

---

## 3. Entity Catalog

The following entities exist in the system. Each entity is classified by its primary storage tier and the module that owns it.

| Entity | Primary Tier | Owning Module | Description |
|---|---|---|---|
| `Participant` | Tier 3 | `gateway` / admin | A firm or individual authorized to submit orders. The multi-tenancy root. |
| `ParticipantCredential` | Tier 3 | `gateway` | Hashed session token used for TCP connection authentication. |
| `Instrument` | Tier 3 | admin / `engine` (read-only cache) | A tradeable symbol with its tick size, lot size, and trading state. |
| `TradingSession` | Tier 3 | admin | A named time window during which an instrument is open for trading. |
| `Order` | Tier 1 (live) / Tier 2 (journal) / Tier 3 (audit) | `engine` (live), `audit-writer` (Tier 3) | A resting or in-flight order. The Tier 3 record is the post-trade audit copy. |
| `Trade` | Tier 1 (transient) / Tier 2 (journal) / Tier 3 (audit) | `engine` (live), `audit-writer` (Tier 3) | A matched execution between an aggressor and a passive order. |
| `ExecutionReport` | Tier 2 (journal) / Tier 3 (audit) | `engine` (live), `audit-writer` (Tier 3) | A state-change notification for an order (ack, fill, cancel, reject). |
| `ClientSession` | Tier 1 (in-memory) | `gateway` | An active TCP connection from a participant. Ephemeral; not persisted. |
| `RiskLimit` | Tier 3 | admin | Per-participant or per-instrument position and order-rate limits. |
| `JournalSegment` | Tier 2 (filesystem) | `sequencer` | A physical journal file segment. Metadata tracked in Tier 3 for operational visibility. |
| `SnapshotRecord` | Tier 2 (filesystem) | `snapshot-writer` | A point-in-time engine state snapshot. Metadata tracked in Tier 3. |
| `AuditEvent` | Tier 3 | `audit-writer` | Immutable append-only record of every significant system event. |

---

## 4. Relational Schema

All tables live in the `velox` schema. The schema is created with `CREATE SCHEMA IF NOT EXISTS velox`. All timestamps are `TIMESTAMPTZ` stored in UTC. All monetary prices are stored as `BIGINT` representing price × 10,000 (four implied decimal places), matching the wire protocol encoding exactly — no floating-point types appear anywhere in the schema.

---

### 4.1 `velox.participant`

The root entity for multi-tenancy. Every order, trade, session, and risk limit is owned by a participant.

```sql
CREATE TABLE velox.participant (
    participant_id      BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    participant_code    VARCHAR(16)     NOT NULL,   -- short mnemonic, e.g. "ACME01"
    participant_name    VARCHAR(128)    NOT NULL,
    participant_type    VARCHAR(16)     NOT NULL     -- 'FIRM', 'MARKET_MAKER', 'RETAIL'
                            CHECK (participant_type IN ('FIRM', 'MARKET_MAKER', 'RETAIL')),
    status              VARCHAR(16)     NOT NULL DEFAULT 'ACTIVE'
                            CHECK (status IN ('ACTIVE', 'SUSPENDED', 'TERMINATED')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),

    CONSTRAINT participant_code_unique UNIQUE (participant_code)
);

COMMENT ON TABLE velox.participant IS
    'Root multi-tenancy entity. Every order and trade is owned by a participant.';
COMMENT ON COLUMN velox.participant.participant_id IS
    'Stable surrogate key. Carried on every wire-protocol message as participantId (long).';
COMMENT ON COLUMN velox.participant.participant_code IS
    'Human-readable short code. Used in administrative tooling and reports.';
```

---

### 4.2 `velox.participant_credential`

Stores the authentication material for TCP gateway connections. One participant may have multiple active credentials (e.g., for key rotation).

```sql
CREATE TABLE velox.participant_credential (
    credential_id       BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    participant_id      BIGINT          NOT NULL
                            REFERENCES velox.participant (participant_id)
                            ON DELETE CASCADE,
    token_hash          BYTEA           NOT NULL,   -- SHA-256 of the raw session token
    token_salt          BYTEA           NOT NULL,   -- 16-byte random salt
    label               VARCHAR(64),                -- optional human label, e.g. "prod-key-2"
    status              VARCHAR(16)     NOT NULL DEFAULT 'ACTIVE'
                            CHECK (status IN ('ACTIVE', 'REVOKED')),
    valid_from          TIMESTAMPTZ     NOT NULL DEFAULT now(),
    valid_until         TIMESTAMPTZ,                -- NULL = no expiry
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),
    revoked_at          TIMESTAMPTZ,

    CONSTRAINT credential_token_hash_unique UNIQUE (token_hash)
);

COMMENT ON TABLE velox.participant_credential IS
    'Authentication tokens for TCP gateway connections. '
    'Loaded into AuthHandler in-memory cache at startup and on credential change events.';
COMMENT ON COLUMN velox.participant_credential.token_hash IS
    'SHA-256(salt || raw_token). The raw token is never stored.';
```

---

### 4.3 `velox.instrument`

Defines every tradeable symbol. The matching engine holds an in-memory read-only cache of this table, loaded at startup. Changes require a controlled restart or a hot-reload signal (Phase 2).

```sql
CREATE TABLE velox.instrument (
    instrument_id       INT             GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    symbol              VARCHAR(16)     NOT NULL,
    instrument_name     VARCHAR(128)    NOT NULL,
    instrument_type     VARCHAR(16)     NOT NULL
                            CHECK (instrument_type IN ('EQUITY', 'FUTURE', 'OPTION', 'CRYPTO')),
    base_currency       CHAR(3)         NOT NULL,   -- ISO 4217
    quote_currency      CHAR(3)         NOT NULL,
    tick_size           BIGINT          NOT NULL     -- minimum price increment × 10,000
                            CHECK (tick_size > 0),
    lot_size            BIGINT          NOT NULL     -- minimum order quantity
                            CHECK (lot_size > 0),
    max_order_qty       BIGINT          NOT NULL
                            CHECK (max_order_qty > 0),
    price_band_pct      NUMERIC(5,2),               -- NULL = no price band; e.g. 5.00 = ±5%
    trading_state       VARCHAR(16)     NOT NULL DEFAULT 'CLOSED'
                            CHECK (trading_state IN ('PRE_OPEN', 'OPEN', 'HALTED', 'CLOSED')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),

    CONSTRAINT instrument_symbol_unique UNIQUE (symbol)
);

COMMENT ON TABLE velox.instrument IS
    'Tradeable symbols. Cached in-memory by the matching engine at startup. '
    'instrument_id (INT) is carried on every wire-protocol message as instrumentId.';
COMMENT ON COLUMN velox.instrument.tick_size IS
    'Stored as price × 10,000. A tick of $0.01 is stored as 100.';
COMMENT ON COLUMN velox.instrument.price_band_pct IS
    'If set, orders priced outside ±price_band_pct% of the last trade price are rejected '
    'by the gateway before reaching the matching engine.';
```

---

### 4.4 `velox.trading_session`

Named time windows during which an instrument is open for trading. The gateway uses this to reject orders submitted outside session hours.

```sql
CREATE TABLE velox.trading_session (
    session_id          BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    instrument_id       INT             NOT NULL
                            REFERENCES velox.instrument (instrument_id),
    session_name        VARCHAR(64)     NOT NULL,   -- e.g. "REGULAR", "PRE_MARKET"
    session_date        DATE            NOT NULL,
    open_time           TIMESTAMPTZ     NOT NULL,
    close_time          TIMESTAMPTZ     NOT NULL,
    status              VARCHAR(16)     NOT NULL DEFAULT 'SCHEDULED'
                            CHECK (status IN ('SCHEDULED', 'OPEN', 'CLOSED', 'CANCELLED')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),

    CONSTRAINT session_open_before_close CHECK (open_time < close_time),
    CONSTRAINT session_instrument_date_name_unique
        UNIQUE (instrument_id, session_date, session_name)
);
```

---

### 4.5 `velox.order_audit`

The post-trade audit record for every order that entered the system. This is **not** the live order state — that lives in Tier 1. This is the immutable historical record written by the `audit-writer` service after the fact, sourced from the outbound SPSC ring buffer.

The table name is `order_audit` rather than `order` because `ORDER` is a reserved word in SQL, and because the semantic distinction is important: this is an audit copy, not the authoritative live state.

```sql
CREATE TABLE velox.order_audit (
    -- Surrogate key for the audit record itself
    audit_id            BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,

    -- Order identity (matches wire protocol)
    order_id            BIGINT          NOT NULL,
    global_seq          BIGINT          NOT NULL,   -- sequencer-assigned sequence number
    client_seq          BIGINT          NOT NULL,   -- client-assigned sequence number
    participant_id      BIGINT          NOT NULL
                            REFERENCES velox.participant (participant_id),
    instrument_id       INT             NOT NULL
                            REFERENCES velox.instrument (instrument_id),

    -- Order parameters (as submitted)
    side                CHAR(1)         NOT NULL    CHECK (side IN ('B', 'S')),
    order_type          VARCHAR(16)     NOT NULL    CHECK (order_type IN ('LIMIT', 'MARKET', 'IOC')),
    price               BIGINT,                     -- NULL for MARKET orders; × 10,000
    quantity            BIGINT          NOT NULL    CHECK (quantity > 0),
    time_in_force       VARCHAR(8)      NOT NULL    CHECK (time_in_force IN ('DAY', 'GTC', 'IOC', 'FOK')),

    -- Lifecycle timestamps
    submitted_at        TIMESTAMPTZ     NOT NULL,   -- when gateway received the frame
    accepted_at         TIMESTAMPTZ,                -- when matching engine processed NEW_ACK
    terminal_at         TIMESTAMPTZ,                -- when order reached terminal state

    -- Terminal state
    terminal_status     VARCHAR(16)
                            CHECK (terminal_status IN (
                                'FILLED', 'PARTIALLY_FILLED_CANCELLED',
                                'CANCELLED', 'REJECTED', 'EXPIRED')),
    filled_qty          BIGINT          NOT NULL DEFAULT 0,
    avg_fill_price      BIGINT,                     -- weighted average; × 10,000; NULL if unfilled

    -- Self-trade prevention
    stp_action          VARCHAR(16)
                            CHECK (stp_action IN (
                                'NONE', 'CANCEL_AGGRESSOR', 'CANCEL_PASSIVE', 'CANCEL_BOTH')),

    -- Reject reason (populated only if terminal_status = 'REJECTED')
    reject_reason       VARCHAR(64),

    CONSTRAINT order_audit_order_id_unique UNIQUE (order_id),
    CONSTRAINT order_audit_global_seq_unique UNIQUE (global_seq)
);

COMMENT ON TABLE velox.order_audit IS
    'Immutable post-trade audit record for every order. Written by audit-writer service '
    'from the outbound SPSC ring buffer. Not the live order state (which is in-memory only).';
COMMENT ON COLUMN velox.order_audit.global_seq IS
    'The sequencer-assigned global sequence number. Monotonically increasing. '
    'Enables exact reconstruction of event ordering across all instruments.';
COMMENT ON COLUMN velox.order_audit.price IS
    'Stored as price × 10,000. Matches wire protocol encoding exactly. '
    'NULL for MARKET orders.';
```

---

### 4.6 `velox.execution_report`

Every execution report emitted by the matching engine, in order. This is the complete lifecycle history of every order: acknowledgement, partial fills, final fill, cancellation, or rejection.

```sql
CREATE TABLE velox.execution_report (
    exec_report_id      BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    order_id            BIGINT          NOT NULL
                            REFERENCES velox.order_audit (order_id),
    participant_id      BIGINT          NOT NULL
                            REFERENCES velox.participant (participant_id),
    instrument_id       INT             NOT NULL
                            REFERENCES velox.instrument (instrument_id),
    global_seq          BIGINT          NOT NULL,   -- engine sequence at time of report
    exec_type           VARCHAR(16)     NOT NULL
                            CHECK (exec_type IN (
                                'NEW', 'PARTIAL_FILL', 'FILL',
                                'CANCELLED', 'REJECTED', 'EXPIRED', 'REPLACED')),
    exec_qty            BIGINT          NOT NULL DEFAULT 0,  -- qty executed in this report
    leaves_qty          BIGINT          NOT NULL,            -- remaining open qty
    trade_price         BIGINT,                              -- NULL for non-fill reports; × 10,000
    trade_id            BIGINT,                              -- NULL for non-fill reports
    reported_at         TIMESTAMPTZ     NOT NULL,

    CONSTRAINT exec_report_global_seq_unique UNIQUE (global_seq),
    CONSTRAINT exec_report_fill_has_trade
        CHECK (
            (exec_type IN ('PARTIAL_FILL', 'FILL') AND trade_id IS NOT NULL AND trade_price IS NOT NULL)
            OR exec_type NOT IN ('PARTIAL_FILL', 'FILL')
        ),
    CONSTRAINT exec_report_qty_non_negative
        CHECK (exec_qty >= 0 AND leaves_qty >= 0)
);

COMMENT ON TABLE velox.execution_report IS
    'Complete lifecycle history of every order. One row per execution report emitted '
    'by the matching engine. Ordered by global_seq for deterministic replay verification.';
```

---

### 4.7 `velox.trade`

Every matched execution. Each trade has exactly two sides: an aggressor (the incoming order that crossed the spread) and a passive (the resting order that was hit).

```sql
CREATE TABLE velox.trade (
    trade_id            BIGINT          PRIMARY KEY,    -- engine-assigned; not IDENTITY
    instrument_id       INT             NOT NULL
                            REFERENCES velox.instrument (instrument_id),
    global_seq          BIGINT          NOT NULL,       -- engine sequence at match time
    aggressor_order_id  BIGINT          NOT NULL
                            REFERENCES velox.order_audit (order_id),
    passive_order_id    BIGINT          NOT NULL
                            REFERENCES velox.order_audit (order_id),
    aggressor_participant_id  BIGINT    NOT NULL
                            REFERENCES velox.participant (participant_id),
    passive_participant_id    BIGINT    NOT NULL
                            REFERENCES velox.participant (participant_id),
    aggressor_side      CHAR(1)         NOT NULL    CHECK (aggressor_side IN ('B', 'S')),
    trade_price         BIGINT          NOT NULL    CHECK (trade_price > 0),  -- × 10,000
    trade_qty           BIGINT          NOT NULL    CHECK (trade_qty > 0),
    traded_at           TIMESTAMPTZ     NOT NULL,

    CONSTRAINT trade_global_seq_unique UNIQUE (global_seq),
    CONSTRAINT trade_aggressor_ne_passive
        CHECK (aggressor_order_id <> passive_order_id),
    CONSTRAINT trade_participants_ne_self_trade
        -- Self-trades are prevented by SelfTradePolicy; this is a belt-and-suspenders check.
        CHECK (aggressor_participant_id <> passive_participant_id)
);

COMMENT ON TABLE velox.trade IS
    'Every matched execution. trade_id is assigned by the matching engine '
    '(tradeIdSequence.fetch_add(1)) and carried on the wire protocol. '
    'It is not a database IDENTITY because the engine is the authoritative source.';
COMMENT ON COLUMN velox.trade.trade_price IS
    'The passive order price wins in all matches. Stored × 10,000.';
```

---

### 4.8 `velox.risk_limit`

Per-participant and optionally per-instrument risk controls. Evaluated by the gateway before an order is forwarded to the sequencer. The matching engine never reads this table.

```sql
CREATE TABLE velox.risk_limit (
    risk_limit_id       BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    participant_id      BIGINT          NOT NULL
                            REFERENCES velox.participant (participant_id)
                            ON DELETE CASCADE,
    instrument_id       INT                         -- NULL = applies to all instruments
                            REFERENCES velox.instrument (instrument_id),
    limit_type          VARCHAR(32)     NOT NULL
                            CHECK (limit_type IN (
                                'MAX_ORDER_QTY',        -- single order quantity cap
                                'MAX_OPEN_ORDERS',      -- count of resting orders
                                'MAX_NOTIONAL_PER_ORDER', -- price × qty cap
                                'MAX_ORDERS_PER_SECOND',  -- rate limit
                                'MAX_GROSS_POSITION'    -- total filled qty cap
                            )),
    limit_value         BIGINT          NOT NULL    CHECK (limit_value > 0),
    status              VARCHAR(16)     NOT NULL DEFAULT 'ACTIVE'
                            CHECK (status IN ('ACTIVE', 'DISABLED')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),
    updated_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),

    CONSTRAINT risk_limit_participant_instrument_type_unique
        UNIQUE (participant_id, instrument_id, limit_type)
);

COMMENT ON TABLE velox.risk_limit IS
    'Pre-trade risk controls evaluated by the gateway. '
    'instrument_id = NULL means the limit applies across all instruments for the participant. '
    'The matching engine never reads this table.';
```

---

### 4.9 `velox.journal_segment`

Operational metadata about physical journal file segments. The journal files themselves live on the filesystem (Tier 2); this table provides visibility for monitoring and recovery tooling.

```sql
CREATE TABLE velox.journal_segment (
    segment_id          BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    segment_index       INT             NOT NULL,       -- 0-based file index
    filename            VARCHAR(256)    NOT NULL,
    first_global_seq    BIGINT          NOT NULL,
    last_global_seq     BIGINT,                         -- NULL = segment still open
    byte_size           BIGINT,                         -- NULL = segment still open
    status              VARCHAR(16)     NOT NULL DEFAULT 'OPEN'
                            CHECK (status IN ('OPEN', 'CLOSED', 'ARCHIVED', 'CORRUPTED')),
    opened_at           TIMESTAMPTZ     NOT NULL DEFAULT now(),
    closed_at           TIMESTAMPTZ,

    CONSTRAINT journal_segment_index_unique UNIQUE (segment_index),
    CONSTRAINT journal_segment_seq_order
        CHECK (last_global_seq IS NULL OR last_global_seq >= first_global_seq)
);

COMMENT ON TABLE velox.journal_segment IS
    'Operational metadata for journal file segments. '
    'The journal files are the authoritative Tier 2 store; '
    'this table is a derived operational view for monitoring and recovery tooling.';
```

---

### 4.10 `velox.snapshot_record`

Operational metadata about engine state snapshots. Mirrors the filesystem (Tier 2) for operational visibility.

```sql
CREATE TABLE velox.snapshot_record (
    snapshot_id         BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    filename            VARCHAR(256)    NOT NULL,
    global_seq          BIGINT          NOT NULL,       -- engine seq at snapshot time
    order_count         INT             NOT NULL,       -- resting orders at snapshot time
    byte_size           BIGINT          NOT NULL,
    crc32               BIGINT          NOT NULL,       -- stored as unsigned 32-bit in a BIGINT
    status              VARCHAR(16)     NOT NULL DEFAULT 'VALID'
                            CHECK (status IN ('VALID', 'CORRUPTED', 'SUPERSEDED')),
    created_at          TIMESTAMPTZ     NOT NULL DEFAULT now(),

    CONSTRAINT snapshot_global_seq_unique UNIQUE (global_seq)
);

COMMENT ON TABLE velox.snapshot_record IS
    'Operational metadata for engine state snapshots. '
    'RecoveryService selects the most recent VALID snapshot by global_seq '
    'to minimize journal replay length on restart.';
```

---

### 4.11 `velox.audit_event`

An immutable, append-only log of every significant system event. This is the compliance and operations record, distinct from the execution reports (which are trading events). Audit events cover authentication, configuration changes, risk-limit breaches, and system lifecycle events.

```sql
CREATE TABLE velox.audit_event (
    audit_event_id      BIGINT          GENERATED ALWAYS AS IDENTITY PRIMARY KEY,
    event_type          VARCHAR(64)     NOT NULL,
    -- Examples: 'PARTICIPANT_LOGIN', 'PARTICIPANT_LOGOUT', 'CREDENTIAL_REVOKED',
    --           'RISK_LIMIT_BREACHED', 'INSTRUMENT_HALTED', 'ENGINE_STARTED',
    --           'ENGINE_STOPPED', 'JOURNAL_SEGMENT_ROLLED', 'SNAPSHOT_WRITTEN'
    participant_id      BIGINT                          -- NULL for system events
                            REFERENCES velox.participant (participant_id),
    instrument_id       INT                             -- NULL for non-instrument events
                            REFERENCES velox.instrument (instrument_id),
    session_id          BIGINT,                         -- gateway session ID if applicable
    global_seq          BIGINT,                         -- engine seq if applicable
    detail              JSONB,                          -- event-specific structured detail
    severity            VARCHAR(8)      NOT NULL DEFAULT 'INFO'
                            CHECK (severity IN ('DEBUG', 'INFO', 'WARN', 'ERROR', 'CRITICAL')),
    occurred_at         TIMESTAMPTZ     NOT NULL DEFAULT now()
);

-- Audit events are never updated or deleted. No UPDATE or DELETE privileges granted.
COMMENT ON TABLE velox.audit_event IS
    'Immutable append-only compliance and operations log. '
    'No row in this table is ever updated or deleted. '
    'Partitioned by occurred_at (monthly) for retention management.';
```

---

## 5. Entity Relationships

### 5.1 Entity-Relationship Diagram

```
velox.participant
    │  1
    │  ├─────────────────────────────────────────────────────────────────┐
    │  │                                                                 │
    │  │ N                                                               │ N
    │  ▼                                                                 ▼
velox.participant_credential                                    velox.risk_limit
                                                                    │
                                                                    │ N (instrument_id nullable)
                                                                    │
                                                                    ▼
velox.participant ──────────────────────────────────── velox.instrument
    │  1                                                     │  1
    │                                                        │
    │  N                                                     │  N
    ▼                                                        ▼
velox.order_audit ──────────────────────────── velox.trading_session
    │  1
    │
    │  N
    ▼
velox.execution_report
    │
    │  N (trade_id FK, nullable)
    │
    ▼
velox.trade
    │  1 (aggressor_order_id)
    │  1 (passive_order_id)
    │
    └──────────────────────────────────────────────────────────────────
                                                                      │
                                                                      ▼
                                                              velox.order_audit
                                                              (two FK references
                                                               per trade row)

velox.journal_segment   (no FK — operational metadata only)
velox.snapshot_record   (no FK — operational metadata only)
velox.audit_event       (optional FK to participant, instrument)
```

### 5.2 Cardinality Summary

| Relationship | Cardinality | Notes |
|---|---|---|
| `participant` → `participant_credential` | 1:N | One participant, many credentials (key rotation) |
| `participant` → `order_audit` | 1:N | One participant submits many orders |
| `participant` → `risk_limit` | 1:N | One participant has many limit rules |
| `instrument` → `order_audit` | 1:N | One instrument has many orders |
| `instrument` → `trading_session` | 1:N | One instrument has many sessions (one per day) |
| `instrument` → `risk_limit` | 1:N (nullable) | Risk limits may be instrument-scoped |
| `order_audit` → `execution_report` | 1:N | One order produces many execution reports |
| `order_audit` → `trade` (aggressor) | 1:N | One order may be aggressor in many trades |
| `order_audit` → `trade` (passive) | 1:N | One order may be passive in many trades |
| `trade` → `execution_report` | 1:2 | Each trade produces exactly two fill reports |

### 5.3 Key Referential Integrity Notes

**`trade.trade_id` is not a database `IDENTITY`.** The matching engine assigns `trade_id` values from its own `tradeIdSequence` counter. The database accepts the engine-assigned value as the primary key. This is intentional: the engine is the authoritative source of trade identity, and the database is a downstream audit store. The `UNIQUE (global_seq)` constraint on `velox.trade` provides an independent uniqueness guarantee tied to the sequencer's monotonic counter.

**`order_audit.order_id` is not a database `IDENTITY`.** Same reasoning: `order_