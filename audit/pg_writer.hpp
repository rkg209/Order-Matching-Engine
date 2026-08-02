#pragma once

// The only libpq translation unit in this repo (Spec 012, T4). Nothing under engine/, book/,
// ipc/, runtime/, gateway/, or marketdata/ includes this header, and nothing outside the
// VELOX_BUILD_AUDIT_TIER build flag links it -- see CMakeLists.txt.
//
// Failure policy, non-negotiable: Postgres being unreachable is a normal operating state for
// this component, not an error path. Any libpq failure inside writeBatch() rolls back, closes
// the connection, and returns false; it never throws, never partially commits, and never
// advances the caller's checkpoint outside a transaction that actually committed. The caller
// (velox_auditd) is expected to back off and retry the identical batch -- which is safe because
// order_event/trade rows carry natural keys and every INSERT here is ON CONFLICT DO NOTHING.

#include <cstdint>
#include <string>
#include <vector>

#include "audit/audit_replayer.hpp"
#include "sequencer/journal_tailer.hpp"

struct pg_conn;  // opaque libpq PGconn, forward-declared so this header stays libpq-include-free

namespace velox::audit {

class PgWriter {
 public:
    explicit PgWriter(std::string conninfo);
    ~PgWriter();

    PgWriter(const PgWriter&) = delete;
    PgWriter& operator=(const PgWriter&) = delete;

    // Writes order_event rows, trade rows, and the new checkpoint in ONE transaction, with a
    // COPY-into-temp-table-then-INSERT-ON-CONFLICT-DO-NOTHING per table (T4's design: COPY's
    // throughput, per-row idempotency). Returns false on any failure -- connection is already
    // torn down when this returns false; the next call re-establishes it.
    bool writeBatch(std::uint32_t shardId, const std::vector<OrderEventRow>& orderEvents,
                    const std::vector<TradeRow>& trades, const sequencer::Checkpoint& checkpoint);

    // Reads the committed checkpoint for `shardId`. Returns false if there is none yet (a brand
    // new shard) OR the connection failed -- both cases are safe to treat as "start from the
    // beginning of the journal directory": every row this writer inserts is ON CONFLICT DO
    // NOTHING, so replaying already-ingested records is a no-op, never a duplicate.
    bool loadCheckpoint(std::uint32_t shardId, sequencer::Checkpoint& out);

    bool connected() const noexcept;

 private:
    bool ensureConnected();
    void disconnect();
    bool rollbackAndDisconnect();
    bool execOk(const char* sql);
    bool copyOrderEvents(const std::vector<OrderEventRow>& rows);
    bool copyTrades(const std::vector<TradeRow>& rows);
    bool upsertCheckpoint(std::uint32_t shardId, const sequencer::Checkpoint& checkpoint);

    std::string conninfo_;
    pg_conn* conn_ = nullptr;
};

}  // namespace velox::audit
