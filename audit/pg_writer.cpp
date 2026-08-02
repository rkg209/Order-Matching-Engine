#include "audit/pg_writer.hpp"

#include <libpq-fe.h>

#include <cstdio>
#include <cstdlib>
#include <string>

namespace velox::audit {

namespace {

std::string toCopyLine(std::initializer_list<long long> fields) {
    std::string line;
    bool first = true;
    for (long long f : fields) {
        if (!first) {
            line.push_back('\t');
        }
        first = false;
        line += std::to_string(f);
    }
    line.push_back('\n');
    return line;
}

}  // namespace

PgWriter::PgWriter(std::string conninfo) : conninfo_(std::move(conninfo)) {}

PgWriter::~PgWriter() {
    disconnect();
}

bool PgWriter::connected() const noexcept {
    return conn_ != nullptr && PQstatus(reinterpret_cast<PGconn*>(conn_)) == CONNECTION_OK;
}

bool PgWriter::ensureConnected() {
    if (connected()) {
        return true;
    }
    disconnect();
    PGconn* c = PQconnectdb(conninfo_.c_str());
    if (PQstatus(c) != CONNECTION_OK) {
        PQfinish(c);
        return false;
    }
    conn_ = reinterpret_cast<pg_conn*>(c);
    return true;
}

void PgWriter::disconnect() {
    if (conn_ != nullptr) {
        PQfinish(reinterpret_cast<PGconn*>(conn_));
        conn_ = nullptr;
    }
}

bool PgWriter::rollbackAndDisconnect() {
    if (conn_ != nullptr) {
        PGresult* r = PQexec(reinterpret_cast<PGconn*>(conn_), "ROLLBACK");
        PQclear(r);
    }
    disconnect();
    return false;
}

bool PgWriter::execOk(const char* sql) {
    PGresult* r = PQexec(reinterpret_cast<PGconn*>(conn_), sql);
    const ExecStatusType st = PQresultStatus(r);
    const bool ok = (st == PGRES_COMMAND_OK) || (st == PGRES_TUPLES_OK);
    PQclear(r);
    return ok;
}

bool PgWriter::copyOrderEvents(const std::vector<OrderEventRow>& rows) {
    if (rows.empty()) {
        return true;
    }
    PGconn* c = reinterpret_cast<PGconn*>(conn_);
    if (!execOk("CREATE TEMP TABLE stage_order_event (LIKE velox_audit.order_event INCLUDING "
                "DEFAULTS) ON COMMIT DROP")) {
        return false;
    }
    PGresult* copyRes = PQexec(c,
                               "COPY stage_order_event (shard_id, global_seq, kind, order_id, "
                               "new_order_id, participant, side, order_type, price, quantity) "
                               "FROM STDIN");
    if (PQresultStatus(copyRes) != PGRES_COPY_IN) {
        PQclear(copyRes);
        return false;
    }
    PQclear(copyRes);

    for (const OrderEventRow& row : rows) {
        std::string line;
        line += std::to_string(row.shardId);
        line.push_back('\t');
        line += std::to_string(row.globalSeq);
        line.push_back('\t');
        line += std::to_string(static_cast<int>(row.kind));
        line.push_back('\t');
        line += std::to_string(row.orderId);
        line.push_back('\t');
        line += (row.kind == ipc::CommandKind::Replace) ? std::to_string(row.newOrderId)
                                                        : std::string("\\N");
        line.push_back('\t');
        line += std::to_string(row.participant);
        line.push_back('\t');
        line += std::to_string(static_cast<int>(row.side));
        line.push_back('\t');
        line += std::to_string(static_cast<int>(row.orderType));
        line.push_back('\t');
        line += std::to_string(row.price);
        line.push_back('\t');
        line += std::to_string(row.quantity);
        line.push_back('\n');
        if (PQputCopyData(c, line.data(), static_cast<int>(line.size())) != 1) {
            PQputCopyEnd(c, "copy failed");
            return false;
        }
    }
    if (PQputCopyEnd(c, nullptr) != 1) {
        return false;
    }
    PGresult* endRes = PQgetResult(c);
    const bool ok = PQresultStatus(endRes) == PGRES_COMMAND_OK;
    PQclear(endRes);
    if (!ok) {
        return false;
    }

    if (!execOk("INSERT INTO velox_audit.order_event (shard_id, global_seq, kind, order_id, "
                "new_order_id, participant, side, order_type, price, quantity) "
                "SELECT shard_id, global_seq, kind, order_id, new_order_id, participant, side, "
                "order_type, price, quantity FROM stage_order_event ON CONFLICT DO NOTHING")) {
        return false;
    }
    if (!execOk("INSERT INTO velox_audit.participant (participant_id, first_seen_seq) "
                "SELECT participant, MIN(global_seq) FROM stage_order_event GROUP BY participant "
                "ON CONFLICT DO NOTHING")) {
        return false;
    }
    if (!execOk("INSERT INTO velox_audit.instrument (shard_id, first_seen_seq) "
                "SELECT shard_id, MIN(global_seq) FROM stage_order_event GROUP BY shard_id "
                "ON CONFLICT DO NOTHING")) {
        return false;
    }
    return true;
}

bool PgWriter::copyTrades(const std::vector<TradeRow>& rows) {
    if (rows.empty()) {
        return true;
    }
    PGconn* c = reinterpret_cast<PGconn*>(conn_);
    if (!execOk("CREATE TEMP TABLE stage_trade (LIKE velox_audit.trade INCLUDING DEFAULTS) "
                "ON COMMIT DROP")) {
        return false;
    }
    PGresult* copyRes = PQexec(c,
                               "COPY stage_trade (shard_id, trade_id, global_seq, "
                               "aggressor_order_id, passive_order_id, price, quantity, "
                               "aggressor_side) FROM STDIN");
    if (PQresultStatus(copyRes) != PGRES_COPY_IN) {
        PQclear(copyRes);
        return false;
    }
    PQclear(copyRes);

    for (const TradeRow& row : rows) {
        const std::string line = toCopyLine({
            static_cast<long long>(row.shardId),
            static_cast<long long>(row.tradeId),
            static_cast<long long>(row.globalSeq),
            static_cast<long long>(row.aggressorOrderId),
            static_cast<long long>(row.passiveOrderId),
            static_cast<long long>(row.price),
            static_cast<long long>(row.quantity),
            static_cast<long long>(row.aggressorSide),
        });
        if (PQputCopyData(c, line.data(), static_cast<int>(line.size())) != 1) {
            PQputCopyEnd(c, "copy failed");
            return false;
        }
    }
    if (PQputCopyEnd(c, nullptr) != 1) {
        return false;
    }
    PGresult* endRes = PQgetResult(c);
    const bool ok = PQresultStatus(endRes) == PGRES_COMMAND_OK;
    PQclear(endRes);
    if (!ok) {
        return false;
    }

    return execOk(
        "INSERT INTO velox_audit.trade (shard_id, trade_id, global_seq, aggressor_order_id, "
        "passive_order_id, price, quantity, aggressor_side) "
        "SELECT shard_id, trade_id, global_seq, aggressor_order_id, passive_order_id, price, "
        "quantity, aggressor_side FROM stage_trade ON CONFLICT DO NOTHING");
}

bool PgWriter::upsertCheckpoint(std::uint32_t shardId, const sequencer::Checkpoint& checkpoint) {
    char sql[512];
    std::snprintf(sql, sizeof(sql),
                  "INSERT INTO velox_audit.ingest_checkpoint (shard_id, "
                  "segment_created_counter, segment_offset, last_global_seq, updated_at) "
                  "VALUES (%u, %llu, %llu, %lld, now()) "
                  "ON CONFLICT (shard_id) DO UPDATE SET "
                  "segment_created_counter = EXCLUDED.segment_created_counter, "
                  "segment_offset = EXCLUDED.segment_offset, "
                  "last_global_seq = EXCLUDED.last_global_seq, "
                  "updated_at = now()",
                  shardId, static_cast<unsigned long long>(checkpoint.createdCounter),
                  static_cast<unsigned long long>(checkpoint.offset),
                  static_cast<long long>(checkpoint.lastGoodSeq));
    return execOk(sql);
}

bool PgWriter::loadCheckpoint(std::uint32_t shardId, sequencer::Checkpoint& out) {
    if (!ensureConnected()) {
        return false;
    }
    char sql[256];
    std::snprintf(sql, sizeof(sql),
                  "SELECT segment_created_counter, segment_offset, last_global_seq FROM "
                  "velox_audit.ingest_checkpoint WHERE shard_id = %u",
                  shardId);
    PGresult* r = PQexec(reinterpret_cast<PGconn*>(conn_), sql);
    if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) == 0) {
        PQclear(r);
        return false;
    }
    out.createdCounter = std::strtoull(PQgetvalue(r, 0, 0), nullptr, 10);
    out.offset = static_cast<std::size_t>(std::strtoull(PQgetvalue(r, 0, 1), nullptr, 10));
    out.lastGoodSeq = static_cast<Seq>(std::strtoll(PQgetvalue(r, 0, 2), nullptr, 10));
    PQclear(r);
    return true;
}

bool PgWriter::writeBatch(std::uint32_t shardId, const std::vector<OrderEventRow>& orderEvents,
                          const std::vector<TradeRow>& trades,
                          const sequencer::Checkpoint& checkpoint) {
    if (!ensureConnected()) {
        return false;
    }
    if (!execOk("BEGIN")) {
        return rollbackAndDisconnect();
    }
    if (!copyOrderEvents(orderEvents)) {
        return rollbackAndDisconnect();
    }
    if (!copyTrades(trades)) {
        return rollbackAndDisconnect();
    }
    if (!upsertCheckpoint(shardId, checkpoint)) {
        return rollbackAndDisconnect();
    }
    if (!execOk("COMMIT")) {
        return rollbackAndDisconnect();
    }
    return true;
}

}  // namespace velox::audit
