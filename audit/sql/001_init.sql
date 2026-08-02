-- Velox Tier-3 audit tier schema (Spec 012, T3).
--
-- velox_audit is a DERIVED, QUERYABLE PROJECTION of the segmented journal, produced by
-- `velox_auditd` tailing <root>/shard-N/journal. It is not durability and it is not the source
-- of truth -- the journal is. Everything here is append-only: there is no mutable `orders`
-- table, because an audit store you can UPDATE is one whose history can be rewritten, which
-- defeats the point. Current order state is a VIEW over order_event + trade, not a row.
--
-- Ordering key: `global_seq`, not a timestamp. Nothing in the engine (ipc::Command, Trade,
-- OutboundEvent) carries a wall-clock time -- globalSeq is deliberately ring-arrival order, kept
-- clock-free so the engine stays deterministic (constitution P4). `ingested_at` below is exactly
-- what its name says: when velox_auditd wrote the row. It is NOT when the trade happened. Do not
-- rename it to `traded_at` -- that would fabricate an event time this schema never had.
--
-- Load with: psql -f audit/sql/001_init.sql <db>
--
-- Deliberately absent, and why:
--   - participant_credential : secrets do not belong in a derived store; the gateway alone owns
--                              `--creds`, and this schema never sees it.
--   - risk_limit / trading_session / audit_event : nothing in the engine currently emits any of
--                              these; a table with no producer would ship permanently empty.
--   - partitioning            : premature for a single-box audit tier; revisit if this ever needs
--                              to span more than one Postgres instance.
--   - a velox_telemetry schema: atomic hot-path counters are a separate concern from this
--                              journal-derived projection and have no natural row shape here.

CREATE SCHEMA IF NOT EXISTS velox_audit;

-- One row per journal command (New / Cancel / Replace), append-only.
--
-- order_id / price / quantity describe: the order as submitted (New); the id being removed
-- (Cancel); the REPLACEMENT (Replace) -- ipc::Command carries only one price/quantity slot, and
-- for Replace that slot is always the new order's, never the old one's. new_order_id is set only
-- for Replace (NULL otherwise -- 0 is not used as a sentinel here, unlike the in-process
-- OrderEventRow, because SQL NULL is the honest way to say "not applicable").
CREATE TABLE IF NOT EXISTS velox_audit.order_event (
    shard_id      INTEGER     NOT NULL,
    global_seq    BIGINT      NOT NULL,
    kind          SMALLINT    NOT NULL,   -- ipc::CommandKind: 0=New, 1=Cancel, 2=Replace
    order_id      BIGINT      NOT NULL,
    new_order_id  BIGINT,                 -- Replace only
    participant   BIGINT      NOT NULL,
    side          SMALLINT    NOT NULL,   -- 0=Buy, 1=Sell
    order_type    SMALLINT    NOT NULL,   -- engine::OrderType: 0=Limit,1=Market,2=Ioc,3=Fok
    price         BIGINT      NOT NULL,   -- scaled x10000, matching the engine; ignored for Market
    quantity      BIGINT      NOT NULL,
    ingested_at   TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (shard_id, global_seq)
);

-- One row per executed trade, append-only. trade_id is Trade::id (a deterministic monotonic
-- counter minted by the engine -- never a UUID, never derived from a clock), unique per shard.
CREATE TABLE IF NOT EXISTS velox_audit.trade (
    shard_id           INTEGER     NOT NULL,
    trade_id           BIGINT      NOT NULL,
    global_seq         BIGINT      NOT NULL,  -- the order_event that produced this trade
    aggressor_order_id BIGINT      NOT NULL,
    passive_order_id   BIGINT      NOT NULL,
    price              BIGINT      NOT NULL,  -- the RESTING order's price -- see engine/trade.hpp
    quantity           BIGINT      NOT NULL,
    aggressor_side     SMALLINT    NOT NULL,  -- 0=Buy, 1=Sell
    ingested_at        TIMESTAMPTZ NOT NULL DEFAULT now(),
    PRIMARY KEY (shard_id, trade_id)
);

CREATE INDEX IF NOT EXISTS trade_global_seq_idx ON velox_audit.trade (shard_id, global_seq);

-- Derived: every participant id ever seen, and the first order_event it appeared in. No
-- credentials, no PII -- that stays entirely with the gateway's `--creds` file.
CREATE TABLE IF NOT EXISTS velox_audit.participant (
    participant_id  BIGINT PRIMARY KEY,
    first_seen_seq  BIGINT NOT NULL
);

-- Derived: every (shard, instrument) pair ever seen. No tick/lot-size metadata -- the engine's
-- BookConfig is the source of truth for that, and it is not journaled per-order.
CREATE TABLE IF NOT EXISTS velox_audit.instrument (
    shard_id        INTEGER PRIMARY KEY,
    first_seen_seq  BIGINT  NOT NULL
);

-- Exactly-once ingest without distributed transactions: updated in the SAME transaction as the
-- batch it covers (T4). Rows in order_event/trade carry natural keys derived from the journal,
-- so a replay after a crash is idempotent via ON CONFLICT DO NOTHING; this checkpoint is what
-- lets velox_auditd know where to resume instead of re-tailing from segment 0 every restart.
CREATE TABLE IF NOT EXISTS velox_audit.ingest_checkpoint (
    shard_id                INTEGER PRIMARY KEY,
    segment_created_counter BIGINT NOT NULL DEFAULT 0,
    segment_offset          BIGINT NOT NULL DEFAULT 0,
    last_global_seq         BIGINT NOT NULL DEFAULT 0,
    updated_at              TIMESTAMPTZ NOT NULL DEFAULT now()
);

-- Current order state, derived rather than stored: a New starts an order at its submitted
-- quantity; each trade against it reduces the outstanding quantity; a Cancel or a Replace's
-- implicit cancel-of-old removes it from "live". This view has no notion of "filled" quantity
-- beyond what trade rows reconstruct -- there is no mutable order row anywhere to fall out of
-- sync with it.
CREATE OR REPLACE VIEW velox_audit.order_current AS
WITH filled AS (
    SELECT shard_id, aggressor_order_id AS order_id, SUM(quantity) AS filled_qty
    FROM velox_audit.trade
    GROUP BY shard_id, aggressor_order_id
    UNION ALL
    SELECT shard_id, passive_order_id AS order_id, SUM(quantity) AS filled_qty
    FROM velox_audit.trade
    GROUP BY shard_id, passive_order_id
),
filled_totals AS (
    SELECT shard_id, order_id, SUM(filled_qty) AS filled_qty
    FROM filled
    GROUP BY shard_id, order_id
),
cancelled AS (
    SELECT shard_id, order_id, TRUE AS is_cancelled
    FROM velox_audit.order_event
    WHERE kind = 1  -- Cancel
    UNION
    SELECT shard_id, order_id, TRUE AS is_cancelled
    FROM velox_audit.order_event
    WHERE kind = 2  -- Replace: old_id (order_id) is cancelled by the replace
)
SELECT
    oe.shard_id,
    oe.order_id,
    oe.participant,
    oe.side,
    oe.order_type,
    oe.price,
    oe.quantity                                 AS original_quantity,
    COALESCE(ft.filled_qty, 0)                   AS filled_quantity,
    oe.quantity - COALESCE(ft.filled_qty, 0)      AS remaining_quantity,
    (c.is_cancelled IS TRUE)                      AS is_cancelled,
    oe.global_seq                                 AS submitted_seq
FROM velox_audit.order_event oe
LEFT JOIN filled_totals ft ON ft.shard_id = oe.shard_id AND ft.order_id = oe.order_id
LEFT JOIN cancelled c       ON c.shard_id = oe.shard_id AND c.order_id = oe.order_id
WHERE oe.kind = 0;  -- one logical row per originally-submitted order (New)
