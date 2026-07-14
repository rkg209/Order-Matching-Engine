```sql
-- =============================================================================
-- Velox Matching Engine — Relational Schema
-- =============================================================================
-- Database:   PostgreSQL 16
-- Encoding:   UTF-8
-- Collation:  C (deterministic, byte-order comparison for identifiers)
--
-- Design Rationale
-- ----------------
-- This schema persists the durable, auditable record of everything that passes
-- through the Velox matching engine.  The in-memory hot path owns no persistent
-- state; this schema is the system of record for:
--
--   1. Participants and credentials (read at gateway startup / auth).
--   2. Instruments and their tick/lot rules (read at engine startup).
--   3. The command journal mirror (written by the Sequencer after fsync).
--   4. Orders and their full lifecycle (written by the ExecutionReportRouter).
--   5. Trades (written by the ExecutionReportRouter).
--   6. Execution reports (complete audit trail, one row per report).
--   7. Market-data snapshots (L2 book state at configurable intervals).
--   8. Telemetry / latency samples (written by LatencyPublisher).
--   9. Snapshots manifest (tracks which binary snapshot files exist on disk).
--  10. Session audit log (gateway connect / disconnect events).
--
-- The schema is intentionally write-heavy and append-oriented.  Rows in the
-- journal, execution-report, and trade tables are never updated or deleted
-- during normal operation; they are the immutable audit log.  The `orders`
-- table is the one exception: `remaining_qty`, `status`, and `updated_at` are
-- updated in place as execution reports arrive, giving operators a live view of
-- open interest without joining across many execution-report rows.
--
-- All monetary values (prices) are stored as BIGINT representing the raw
-- wire-protocol encoding (price × 10,000), matching the engine's internal
-- representation exactly.  No NUMERIC/DECIMAL is used on hot-path tables to
-- avoid conversion overhead in the reporting layer.
--
-- Migrations
-- ----------
-- Migrations are plain numbered SQL files in migrations/, applied via a small
-- shell script (scripts/migrate.sh) -- no JVM dependency required.
-- Naming convention:
--   V{version}__{description}.sql   — versioned, applied once, never edited.
--   R__{description}.sql            — repeatable, applied when checksum changes
--                                     (views, functions only).
--   U{version}__{description}.sql   — undo scripts (kept alongside V scripts).
--
-- This file represents the baseline: V1__baseline_schema.sql.
-- Every subsequent structural change gets its own V{n}__ file.
-- Schema changes to hot-path-adjacent tables (orders, trades, exec_reports,
-- journal_commands) require a corresponding architecture review because the
-- ExecutionReportRouter writes to them synchronously off the hot path.
-- =============================================================================

-- ---------------------------------------------------------------------------
-- Extensions
-- ---------------------------------------------------------------------------

CREATE EXTENSION IF NOT EXISTS pgcrypto;   -- gen_random_uuid(), crypt()
CREATE EXTENSION IF NOT EXISTS pg_stat_statements;  -- query telemetry

-- ---------------------------------------------------------------------------
-- Schema namespaces
-- ---------------------------------------------------------------------------

CREATE SCHEMA IF NOT EXISTS velox;          -- all application objects
CREATE SCHEMA IF NOT EXISTS velox_audit;    -- immutable audit tables
CREATE SCHEMA IF NOT EXISTS velox_telemetry; -- latency / counter samples

SET search_path = velox, velox_audit, velox_telemetry, public;

-- ===========================================================================
-- SECTION 1 — Reference / Configuration Tables
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 1.1  participants
-- ---------------------------------------------------------------------------
-- One row per trading firm / client that may connect to the gateway.
-- The gateway's AuthHandler loads this table into its in-memory credential
-- store at startup and refreshes it on SIGHUP.
-- ---------------------------------------------------------------------------

CREATE TABLE velox.participants (
    participant_id          BIGINT          NOT NULL,
    participant_code        VARCHAR(16)     NOT NULL,   -- human-readable mnemonic
    display_name            VARCHAR(128)    NOT NULL,
    hashed_token            BYTEA           NOT NULL,   -- bcrypt hash of the session token
    token_salt              BYTEA           NOT NULL,   -- stored separately for clarity
    is_active               BOOLEAN         NOT NULL DEFAULT TRUE,
    max_connections         SMALLINT        NOT NULL DEFAULT 10,
    max_order_rate_per_sec  INT             NOT NULL DEFAULT 10000,
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT now(),
    updated_at              TIMESTAMPTZ     NOT NULL DEFAULT now(),
    deactivated_at          TIMESTAMPTZ,

    CONSTRAINT pk_participants
        PRIMARY KEY (participant_id),

    CONSTRAINT uq_participants_code
        UNIQUE (participant_code),

    CONSTRAINT ck_participants_max_connections
        CHECK (max_connections BETWEEN 1 AND 1000),

    CONSTRAINT ck_participants_max_order_rate
        CHECK (max_order_rate_per_sec BETWEEN 1 AND 10000000)
);

COMMENT ON TABLE  velox.participants                    IS 'Trading firms / clients authorised to connect to the order gateway.';
COMMENT ON COLUMN velox.participants.participant_id     IS 'Stable numeric identifier; matches the participantId field on the wire protocol.';
COMMENT ON COLUMN velox.participants.hashed_token       IS 'bcrypt hash of the plaintext session token presented in the LOGIN frame.';
COMMENT ON COLUMN velox.participants.max_order_rate_per_sec IS 'Soft rate limit enforced by the gateway; hard limit is the ring-buffer backpressure.';

CREATE INDEX ix_participants_active
    ON velox.participants (is_active)
    WHERE is_active = TRUE;

-- ---------------------------------------------------------------------------
-- 1.2  instruments
-- ---------------------------------------------------------------------------
-- One row per tradeable instrument.  Loaded into the engine at startup.
-- The engine creates one OrderBook per active instrument.
-- ---------------------------------------------------------------------------

CREATE TABLE velox.instruments (
    instrument_id           INT             NOT NULL,
    symbol                  VARCHAR(32)     NOT NULL,
    isin                    CHAR(12),
    display_name            VARCHAR(128)    NOT NULL,
    asset_class             VARCHAR(32)     NOT NULL,   -- EQUITY, FUTURE, OPTION, CRYPTO, FX
    currency                CHAR(3)         NOT NULL,   -- ISO 4217
    tick_size               BIGINT          NOT NULL,   -- minimum price increment × 10,000
    lot_size                BIGINT          NOT NULL DEFAULT 1,  -- minimum order quantity
    min_price               BIGINT          NOT NULL DEFAULT 1,
    max_price               BIGINT          NOT NULL DEFAULT 9999999999999,
    max_order_qty           BIGINT          NOT NULL DEFAULT 1000000000,
    price_decimals          SMALLINT        NOT NULL DEFAULT 4,  -- display hint: raw / 10^price_decimals
    is_active               BOOLEAN         NOT NULL DEFAULT TRUE,
    trading_start           TIME,           -- daily session open (NULL = always open)
    trading_end             TIME,           -- daily session close
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT now(),
    updated_at              TIMESTAMPTZ     NOT NULL DEFAULT now(),

    CONSTRAINT pk_instruments
        PRIMARY KEY (instrument_id),

    CONSTRAINT uq_instruments_symbol
        UNIQUE (symbol),

    CONSTRAINT ck_instruments_asset_class
        CHECK (asset_class IN ('EQUITY','FUTURE','OPTION','CRYPTO','FX','BOND','ETF')),

    CONSTRAINT ck_instruments_tick_size
        CHECK (tick_size > 0),

    CONSTRAINT ck_instruments_lot_size
        CHECK (lot_size > 0),

    CONSTRAINT ck_instruments_price_range
        CHECK (min_price > 0 AND max_price > min_price),

    CONSTRAINT ck_instruments_price_decimals
        CHECK (price_decimals BETWEEN 0 AND 8)
);

COMMENT ON TABLE  velox.instruments                 IS 'Tradeable instruments; one OrderBook is created per active instrument at engine startup.';
COMMENT ON COLUMN velox.instruments.tick_size       IS 'Minimum price increment encoded as raw long (price × 10,000). Must divide evenly into all submitted prices.';
COMMENT ON COLUMN velox.instruments.price_decimals  IS 'Display hint only. The engine always works in raw long units.';

CREATE INDEX ix_instruments_active
    ON velox.instruments (is_active)
    WHERE is_active = TRUE;

CREATE INDEX ix_instruments_symbol
    ON velox.instruments (symbol);

-- ===========================================================================
-- SECTION 2 — Session Audit
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 2.1  gateway_sessions
-- ---------------------------------------------------------------------------
-- One row per TCP connection accepted by the OrderGateway.
-- Written by the GatewayService on connect and updated on disconnect.
-- ---------------------------------------------------------------------------

CREATE TABLE velox_audit.gateway_sessions (
    session_id              BIGINT          NOT NULL GENERATED ALWAYS AS IDENTITY,
    participant_id          BIGINT          NOT NULL,
    remote_address          INET            NOT NULL,
    remote_port             INT             NOT NULL,
    connected_at            TIMESTAMPTZ     NOT NULL DEFAULT now(),
    disconnected_at         TIMESTAMPTZ,
    disconnect_reason       VARCHAR(64),    -- CLEAN_CLOSE, BROKEN_PIPE, AUTH_FAILURE, TIMEOUT
    total_orders_sent       BIGINT          NOT NULL DEFAULT 0,
    total_orders_rejected   BIGINT          NOT NULL DEFAULT 0,
    login_global_seq        BIGINT,         -- globalSeqNum of the LOGIN command
    logout_global_seq       BIGINT,

    CONSTRAINT pk_gateway_sessions
        PRIMARY KEY (session_id),

    CONSTRAINT fk_gateway_sessions_participant
        FOREIGN KEY (participant_id)
        REFERENCES velox.participants (participant_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT ck_gateway_sessions_port
        CHECK (remote_port BETWEEN 1 AND 65535),

    CONSTRAINT ck_gateway_sessions_disconnect_reason
        CHECK (disconnect_reason IN (
            'CLEAN_CLOSE','BROKEN_PIPE','AUTH_FAILURE',
            'TIMEOUT','PROTOCOL_ERROR','SERVER_SHUTDOWN', NULL
        ))
);

COMMENT ON TABLE velox_audit.gateway_sessions IS 'One row per accepted TCP connection. Updated on disconnect. Never deleted.';

CREATE INDEX ix_gateway_sessions_participant
    ON velox_audit.gateway_sessions (participant_id, connected_at DESC);

CREATE INDEX ix_gateway_sessions_connected_at
    ON velox_audit.gateway_sessions (connected_at DESC);

CREATE INDEX ix_gateway_sessions_open
    ON velox_audit.gateway_sessions (participant_id)
    WHERE disconnected_at IS NULL;

-- ===========================================================================
-- SECTION 3 — Command Journal Mirror
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 3.1  journal_commands
-- ---------------------------------------------------------------------------
-- Mirror of the binary journal written by the Sequencer.
-- The Sequencer writes to the binary journal file first (fsync), then
-- publishes to the ring, then asynchronously inserts here.
-- This table enables SQL-based replay auditing and gap detection without
-- reading binary files.  It is append-only; rows are never updated.
-- ---------------------------------------------------------------------------

CREATE TABLE velox_audit.journal_commands (
    global_seq              BIGINT          NOT NULL,
    command_type            SMALLINT        NOT NULL,   -- 1=NEW_ORDER 2=CANCEL 3=CANCEL_REPLACE
    participant_id          BIGINT          NOT NULL,
    session_id              BIGINT          NOT NULL,
    client_seq_num          BIGINT          NOT NULL,
    instrument_id           INT             NOT NULL,
    order_id                BIGINT          NOT NULL,
    side                    CHAR(1),                    -- B=BUY S=SELL NULL for CANCEL
    order_type              CHAR(1),                    -- L=LIMIT M=MARKET
    time_in_force           CHAR(3),                    -- GTC IOC DAY
    price                   BIGINT,                     -- raw long; NULL for MARKET orders
    quantity                BIGINT,
    new_price               BIGINT,                     -- CANCEL_REPLACE only
    new_quantity            BIGINT,                     -- CANCEL_REPLACE only
    received_at             TIMESTAMPTZ     NOT NULL DEFAULT clock_timestamp(),
    journaled_at            TIMESTAMPTZ     NOT NULL,   -- timestamp after fsync confirmed
    payload_bytes           BYTEA           NOT NULL,   -- verbatim wire payload for replay

    CONSTRAINT pk_journal_commands
        PRIMARY KEY (global_seq),

    CONSTRAINT ck_journal_commands_command_type
        CHECK (command_type IN (1, 2, 3)),

    CONSTRAINT ck_journal_commands_side
        CHECK (side IN ('B', 'S') OR side IS NULL),

    CONSTRAINT ck_journal_commands_order_type
        CHECK (order_type IN ('L', 'M') OR order_type IS NULL),

    CONSTRAINT ck_journal_commands_time_in_force
        CHECK (time_in_force IN ('GTC', 'IOC', 'DAY') OR time_in_force IS NULL),

    CONSTRAINT ck_journal_commands_price
        CHECK (price IS NULL OR price > 0),

    CONSTRAINT ck_journal_commands_quantity
        CHECK (quantity IS NULL OR quantity > 0)
) PARTITION BY RANGE (global_seq);

COMMENT ON TABLE  velox_audit.journal_commands              IS 'SQL mirror of the binary Sequencer journal. Append-only. Partitioned by global_seq range for efficient archival.';
COMMENT ON COLUMN velox_audit.journal_commands.global_seq   IS 'Strictly monotonic sequence number assigned by the Sequencer. Primary key and partition key.';
COMMENT ON COLUMN velox_audit.journal_commands.payload_bytes IS 'Verbatim binary command payload as written to the journal file. Enables byte-identical replay verification.';

-- Initial partition covering the first 10 million commands.
-- New partitions are created by the DBA or a scheduled job as global_seq grows.
CREATE TABLE velox_audit.journal_commands_p0
    PARTITION OF velox_audit.journal_commands
    FOR VALUES FROM (0) TO (10000000);

CREATE TABLE velox_audit.journal_commands_p1
    PARTITION OF velox_audit.journal_commands
    FOR VALUES FROM (10000000) TO (20000000);

CREATE INDEX ix_journal_commands_order_id
    ON velox_audit.journal_commands (order_id);

CREATE INDEX ix_journal_commands_participant
    ON velox_audit.journal_commands (participant_id, global_seq);

CREATE INDEX ix_journal_commands_instrument
    ON velox_audit.journal_commands (instrument_id, global_seq);

CREATE INDEX ix_journal_commands_received_at
    ON velox_audit.journal_commands (received_at DESC);

-- ===========================================================================
-- SECTION 4 — Orders
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 4.1  orders
-- ---------------------------------------------------------------------------
-- One row per order submitted to the engine.  This is the only table in the
-- schema that is updated in place (remaining_qty, status, updated_at).
-- The ExecutionReportRouter upserts here on every execution report so that
-- operators have a live view of open interest.
--
-- The full immutable audit trail is in execution_reports (Section 5).
-- ---------------------------------------------------------------------------

CREATE TABLE velox.orders (
    order_id                BIGINT          NOT NULL,
    global_seq              BIGINT          NOT NULL,   -- globalSeqNum of the NEW_ORDER command
    participant_id          BIGINT          NOT NULL,
    session_id              BIGINT          NOT NULL,
    instrument_id           INT             NOT NULL,
    client_seq_num          BIGINT          NOT NULL,
    side                    CHAR(1)         NOT NULL,   -- B=BUY S=SELL
    order_type              CHAR(1)         NOT NULL,   -- L=LIMIT M=MARKET
    time_in_force           CHAR(3)         NOT NULL,   -- GTC IOC DAY
    price                   BIGINT,                     -- NULL for MARKET orders
    original_qty            BIGINT          NOT NULL,
    remaining_qty           BIGINT          NOT NULL,
    executed_qty            BIGINT          NOT NULL DEFAULT 0,
    status                  VARCHAR(16)     NOT NULL DEFAULT 'NEW',
    -- NEW PARTIALLY_FILLED FILLED CANCELLED REJECTED PENDING_CANCEL PENDING_REPLACE
    reject_reason           VARCHAR(64),
    stp_action              VARCHAR(32),                -- CANCEL_AGGRESSOR CANCEL_PASSIVE CANCEL_BOTH ALLOW
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT clock_timestamp(),
    updated_at              TIMESTAMPTZ     NOT NULL DEFAULT clock_timestamp(),
    terminal_at             TIMESTAMPTZ,                -- when status became FILLED/CANCELLED/REJECTED

    CONSTRAINT pk_orders
        PRIMARY KEY (order_id),

    CONSTRAINT fk_orders_participant
        FOREIGN KEY (participant_id)
        REFERENCES velox.participants (participant_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT fk_orders_instrument
        FOREIGN KEY (instrument_id)
        REFERENCES velox.instruments (instrument_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT fk_orders_session
        FOREIGN KEY (session_id)
        REFERENCES velox_audit.gateway_sessions (session_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT ck_orders_side
        CHECK (side IN ('B', 'S')),

    CONSTRAINT ck_orders_order_type
        CHECK (order_type IN ('L', 'M')),

    CONSTRAINT ck_orders_time_in_force
        CHECK (time_in_force IN ('GTC', 'IOC', 'DAY')),

    CONSTRAINT ck_orders_price
        CHECK (price IS NULL OR price > 0),

    CONSTRAINT ck_orders_original_qty
        CHECK (original_qty > 0),

    CONSTRAINT ck_orders_remaining_qty
        CHECK (remaining_qty >= 0 AND remaining_qty <= original_qty),

    CONSTRAINT ck_orders_executed_qty
        CHECK (executed_qty >= 0 AND executed_qty <= original_qty),

    CONSTRAINT ck_orders_qty_consistency
        CHECK (remaining_qty + executed_qty <= original_qty),

    CONSTRAINT ck_orders_status
        CHECK (status IN (
            'NEW','PARTIALLY_FILLED','FILLED',
            'CANCELLED','REJECTED','PENDING_CANCEL','PENDING_REPLACE'
        )),

    CONSTRAINT ck_orders_limit_has_price
        CHECK (order_type <> 'L' OR price IS NOT NULL)
);

COMMENT ON TABLE  velox.orders                  IS 'Live order state. remaining_qty and status are updated in place by the ExecutionReportRouter. Full audit trail is in execution_reports.';
COMMENT ON COLUMN velox.orders.global_seq       IS 'globalSeqNum of the originating NEW_ORDER journal command. Joins to journal_commands.global_seq.';
COMMENT ON COLUMN velox.orders.price            IS 'Raw long encoding: display_price × 10,000. NULL for MARKET orders.';
COMMENT ON COLUMN velox.orders.remaining_qty    IS 'Decremented on each partial fill. Zero when status = FILLED.';

-- Covering index for the open-order query used by the visualizer and risk layer.
CREATE INDEX ix_orders_open
    ON velox.orders (instrument_id, side, price DESC, global_seq)
    WHERE status IN ('NEW', 'PARTIALLY_FILLED');

CREATE INDEX ix_orders_participant_open
    ON velox.orders (participant_id, status, created_at DESC);

CREATE INDEX ix_orders_instrument_created
    ON velox.orders (instrument_id, created_at DESC);

CREATE INDEX ix_orders_global_seq
    ON velox.orders (global_seq);

CREATE INDEX ix_orders_session
    ON velox.orders (session_id);

-- ===========================================================================
-- SECTION 5 — Execution Reports
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 5.1  execution_reports
-- ---------------------------------------------------------------------------
-- Immutable audit log.  One row per execution report emitted by the engine.
-- Multiple rows per order (NEW_ACK, PARTIAL_FILL×N, FILL, CANCELLED, etc.).
-- Written by the ExecutionReportRouter; never updated or deleted.
-- Partitioned by created_at (date) for efficient archival and time-range queries.
-- ---------------------------------------------------------------------------

CREATE TABLE velox_audit.execution_reports (
    exec_report_id          BIGINT          NOT NULL GENERATED ALWAYS AS IDENTITY,
    global_seq              BIGINT          NOT NULL,   -- globalSeqNum that triggered this report
    order_id                BIGINT          NOT NULL,
    participant_id          BIGINT          NOT NULL,
    instrument_id           INT             NOT NULL,
    exec_type               VARCHAR(16)     NOT NULL,
    -- NEW_ACK PARTIAL_FILL FILL CANCELLED REJECTED TRADE_BUST REPLACED
    exec_qty                BIGINT          NOT NULL DEFAULT 0,
    leaves_qty              BIGINT          NOT NULL,
    trade_price             BIGINT,                     -- NULL for non-fill reports
    trade_id                BIGINT,                     -- NULL for non-fill reports
    aggressor_side          CHAR(1),                    -- B or S; NULL for non-fill reports
    reject_reason           VARCHAR(64),
    stp_action              VARCHAR(32),
    created_at              TIMESTAMPTZ     NOT NULL DEFAULT clock_timestamp(),

    CONSTRAINT pk_execution_reports
        PRIMARY KEY (exec_report_id, created_at),   -- composite PK required for partitioning

    CONSTRAINT ck_execution_reports_exec_type
        CHECK (exec_type IN (
            'NEW_ACK','PARTIAL_FILL','FILL',
            'CANCELLED','REJECTED','TRADE_BUST','REPLACED'
        )),

    CONSTRAINT ck_execution_reports_exec_qty
        CHECK (exec_qty >= 0),

    CONSTRAINT ck_execution_reports_leaves_qty
        CHECK (leaves_qty >= 0),

    CONSTRAINT ck_execution_reports_trade_price
        CHECK (trade_price IS NULL OR trade_price > 0),

    CONSTRAINT ck_execution_reports_aggressor_side
        CHECK (aggressor_side IN ('B', 'S') OR aggressor_side IS NULL)
) PARTITION BY RANGE (created_at);

COMMENT ON TABLE  velox_audit.execution_reports             IS 'Immutable execution report log. One row per report emitted by the engine. Partitioned by day.';
COMMENT ON COLUMN velox_audit.execution_reports.global_seq  IS 'globalSeqNum of the command that caused this report. Joins to journal_commands.';
COMMENT ON COLUMN velox_audit.execution_reports.trade_price IS 'Raw long encoding. NULL for non-fill exec types.';

-- Daily partitions — created by a scheduled job (e.g., pg_partman) before each day begins.
-- Two bootstrap partitions shown here; production uses pg_partman for automation.
CREATE TABLE velox_audit.execution_reports_2024_01_01
    PARTITION OF velox_audit.execution_reports
    FOR VALUES FROM ('2024-01-01') TO ('2024-01-02');

CREATE TABLE velox_audit.execution_reports_2024_01_02
    PARTITION OF velox_audit.execution_reports
    FOR VALUES FROM ('2024-01-02') TO ('2024-01-03');

-- Indexes are created on each partition individually by pg_partman.
-- The following are the template indexes applied to every partition:
CREATE INDEX ix_execution_reports_order_id
    ON velox_audit.execution_reports (order_id, created_at DESC);

CREATE INDEX ix_execution_reports_participant
    ON velox_audit.execution_reports (participant_id, created_at DESC);

CREATE INDEX ix_execution_reports_instrument
    ON velox_audit.execution_reports (instrument_id, created_at DESC);

CREATE INDEX ix_execution_reports_trade_id
    ON velox_audit.execution_reports (trade_id)
    WHERE trade_id IS NOT NULL;

CREATE INDEX ix_execution_reports_global_seq
    ON velox_audit.execution_reports (global_seq);

-- ===========================================================================
-- SECTION 6 — Trades
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 6.1  trades
-- ---------------------------------------------------------------------------
-- One row per matched trade (one fill event = one trade row, two exec reports).
-- Written by the ExecutionReportRouter when it processes a FILL or PARTIAL_FILL
-- exec report.  Append-only; never updated.
-- Partitioned by trade_time (date) to mirror execution_reports partitioning.
-- ---------------------------------------------------------------------------

CREATE TABLE velox.trades (
    trade_id                BIGINT          NOT NULL,
    global_seq              BIGINT          NOT NULL,   -- globalSeqNum of the aggressor command
    instrument_id           INT             NOT NULL,
    aggressor_order_id      BIGINT          NOT NULL,
    passive_order_id        BIGINT          NOT NULL,
    aggressor_participant_id BIGINT         NOT NULL,
    passive_participant_id  BIGINT          NOT NULL,
    aggressor_side          CHAR(1)         NOT NULL,   -- B=BUY S=SELL (aggressor's side)
    price                   BIGINT          NOT NULL,   -- passive order's price (price-time priority)
    quantity                BIGINT          NOT NULL,
    trade_time              TIMESTAMPTZ     NOT NULL DEFAULT clock_timestamp(),
    is_busted               BOOLEAN         NOT NULL DEFAULT FALSE,
    busted_at               TIMESTAMPTZ,
    bust_reason             VARCHAR(128),

    CONSTRAINT pk_trades
        PRIMARY KEY (trade_id, trade_time),             -- composite PK for partitioning

    CONSTRAINT fk_trades_instrument
        FOREIGN KEY (instrument_id)
        REFERENCES velox.instruments (instrument_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT fk_trades_aggressor_participant
        FOREIGN KEY (aggressor_participant_id)
        REFERENCES velox.participants (participant_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT fk_trades_passive_participant
        FOREIGN KEY (passive_participant_id)
        REFERENCES velox.participants (participant_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT ck_trades_aggressor_side
        CHECK (aggressor_side IN ('B', 'S')),

    CONSTRAINT ck_trades_price
        CHECK (price > 0),

    CONSTRAINT ck_trades_quantity
        CHECK (quantity > 0),

    CONSTRAINT ck_trades_no_self_trade
        CHECK (aggressor_order_id <> passive_order_id),

    CONSTRAINT ck_trades_bust_consistency
        CHECK (
            (is_busted = FALSE AND busted_at IS NULL AND bust_reason IS NULL)
            OR
            (is_busted = TRUE  AND busted_at IS NOT NULL)
        )
) PARTITION BY RANGE (trade_time);

COMMENT ON TABLE  velox.trades                          IS 'One row per matched trade. Append-only except for bust flag. Partitioned by day.';
COMMENT ON COLUMN velox.trades.price                    IS 'Passive order price wins in price-time priority matching. Raw long encoding.';
COMMENT ON COLUMN velox.trades.aggressor_side           IS 'Side of the incoming (aggressor) order that triggered the match.';
COMMENT ON COLUMN velox.trades.is_busted                IS 'Set TRUE if the trade is subsequently busted by operations. Bust does not delete the row.';

-- Bootstrap partitions (production uses pg_partman).
CREATE TABLE velox.trades_2024_01_01
    PARTITION OF velox.trades
    FOR VALUES FROM ('2024-01-01') TO ('2024-01-02');

CREATE TABLE velox.trades_2024_01_02
    PARTITION OF velox.trades
    FOR VALUES FROM ('2024-01-02') TO ('2024-01-03');

CREATE INDEX ix_trades_instrument_time
    ON velox.trades (instrument_id, trade_time DESC);

CREATE INDEX ix_trades_aggressor_order
    ON velox.trades (aggressor_order_id);

CREATE INDEX ix_trades_passive_order
    ON velox.trades (passive_order_id);

CREATE INDEX ix_trades_aggressor_participant
    ON velox.trades (aggressor_participant_id, trade_time DESC);

CREATE INDEX ix_trades_passive_participant
    ON velox.trades (passive_participant_id, trade_time DESC);

CREATE INDEX ix_trades_global_seq
    ON velox.trades (global_seq);

CREATE INDEX ix_trades_busted
    ON velox.trades (instrument_id, trade_time DESC)
    WHERE is_busted = TRUE;

-- ===========================================================================
-- SECTION 7 — Market-Data Snapshots
-- ===========================================================================

-- ---------------------------------------------------------------------------
-- 7.1  book_snapshots
-- ---------------------------------------------------------------------------
-- Periodic L2 order-book snapshots written by the MarketDataPublisher.
-- Each snapshot captures the top-N price levels on each side at a given
-- globalSeqNum.  Used for replay verification and historical analysis.
-- Not written on the hot path; written asynchronously by the publisher.
-- ---------------------------------------------------------------------------

CREATE TABLE velox.book_snapshots (
    snapshot_id             BIGINT          NOT NULL GENERATED ALWAYS AS IDENTITY,
    instrument_id           INT             NOT NULL,
    global_seq              BIGINT          NOT NULL,   -- seq at which snapshot was taken
    snapshot_time           TIMESTAMPTZ     NOT NULL DEFAULT clock_timestamp(),
    depth                   SMALLINT        NOT NULL,   -- number of levels captured per side
    snapshot_data           JSONB           NOT NULL,
    -- Structure: { "bids": [[price,qty],...], "asks": [[price,qty],...] }
    -- prices are raw long integers matching the wire encoding

    CONSTRAINT pk_book_snapshots
        PRIMARY KEY (snapshot_id),

    CONSTRAINT fk_book_snapshots_instrument
        FOREIGN KEY (instrument_id)
        REFERENCES velox.instruments (instrument_id)
        ON UPDATE CASCADE
        ON DELETE RESTRICT,

    CONSTRAINT ck_book_snapshots_depth
        CHECK (depth BETWEEN 1 AND 100),

    CONSTRAINT ck_book_snapshots_global_seq
        CHECK (global_seq >= 0)
);

COMMENT ON TABLE  velox.book_snapshots              IS 'Periodic L2 book snapshots written by MarketDataPublisher. JSONB payload contains bid/ask price levels as raw long arrays.';
COMMENT ON COLUMN velox.book_snapshots.snapshot_data IS 'JSONB: {"bids":[[price,qty],...],"asks":[[price,qty],...]}. Prices are raw long (× 10,000).';

CREATE INDEX ix_book_snapshots_instrument_seq
    ON velox.book_snapshots (instrument_id, global_seq DESC);

CREATE INDEX ix_book_snapshots_time
    ON velox.book_snapshots (snapshot_time DESC);

-- ---------------------------------------------------------------------------
-- 7.2  trade_ticks
-- ---------------------------------------------------------------------------
-- Lightweight time-series of every trade tick for charting / OHLCV aggregation.
-- Redundant with velox.trades but optimised for time-series queries.
-- Written by MarketDataPublisher asynchronously.
-- ---------------------------------------------------------------------------

CREATE TABLE velox.trade_ticks (
    tick_id                 BIGINT          NOT NULL GENERATED ALWAYS AS IDENTITY,
    trade_id                BIGINT          NOT NULL,
    instrument_id           INT             NOT NULL,
    price                   BIGINT          NOT NULL,
    quantity                BIGINT          NOT NULL,
    aggressor_side          CHAR(1)         NOT NULL,
    global_seq              BIGINT          NOT NULL,
    tick_time               TIMESTAMPTZ     NOT NULL DEFAULT clock_timestamp(),

    CONSTRAINT pk_