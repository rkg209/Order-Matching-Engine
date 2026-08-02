// Spec 012 T7.3: exactly-once ingest without distributed transactions, proven against a real
// Postgres. Skipped entirely (GTEST_SKIP) unless VELOX_TEST_PG_CONNINFO is set, so the `audit`
// label stays green on a box with no database -- this is the one file in tests/audit/ that
// actually needs one.
//
// Setup: `createdb velox_audit_test && psql velox_audit_test -f audit/sql/001_init.sql`, then
//   VELOX_TEST_PG_CONNINFO="dbname=velox_audit_test" ctest --test-dir build -L audit

#include <gtest/gtest.h>
#include <libpq-fe.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "audit/pg_writer.hpp"

using namespace velox;
using namespace velox::audit;

namespace {

// Test-only shard id, chosen to not collide with anything a real deployment would use.
constexpr std::uint32_t kTestShardId = 999999;

std::string requireConninfo() {
    const char* c = std::getenv("VELOX_TEST_PG_CONNINFO");
    if (c == nullptr || c[0] == '\0') {
        return {};
    }
    return c;
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Raw libpq helper for test fixture setup/teardown/counting -- NOT part of the shipped audit
// tier (PgWriter is), a test-only exception noted in tests/CMakeLists.txt.
class RawConn {
 public:
    explicit RawConn(const std::string& conninfo) : conn_(PQconnectdb(conninfo.c_str())) {}
    ~RawConn() { PQfinish(conn_); }

    bool ok() const { return PQstatus(conn_) == CONNECTION_OK; }

    bool exec(const std::string& sql) {
        PGresult* r = PQexec(conn_, sql.c_str());
        const ExecStatusType st = PQresultStatus(r);
        const bool good = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK;
        PQclear(r);
        return good;
    }

    long long countRows(const std::string& table, std::uint32_t shardId) {
        const std::string sql =
            "SELECT COUNT(*) FROM " + table + " WHERE shard_id = " + std::to_string(shardId);
        PGresult* r = PQexec(conn_, sql.c_str());
        if (PQresultStatus(r) != PGRES_TUPLES_OK || PQntuples(r) != 1) {
            PQclear(r);
            return -1;
        }
        const long long n = std::strtoll(PQgetvalue(r, 0, 0), nullptr, 10);
        PQclear(r);
        return n;
    }

    PGconn* raw() { return conn_; }

 private:
    PGconn* conn_;
};

std::vector<OrderEventRow> makeOrderEvents(Seq startSeq, int count) {
    std::vector<OrderEventRow> rows;
    for (int i = 0; i < count; ++i) {
        OrderEventRow r;
        r.globalSeq = startSeq + i;
        r.shardId = kTestShardId;
        r.kind = ipc::CommandKind::New;
        r.orderId = 1000 + startSeq + i;
        r.participant = 1;
        r.side = Side::Buy;
        r.orderType = OrderType::Limit;
        r.price = 100 * kPriceScale;
        r.quantity = 10;
        rows.push_back(r);
    }
    return rows;
}

}  // namespace

TEST(IdempotentIngest, RetryAfterCommitIsANoop) {
    const std::string conninfo = requireConninfo();
    if (conninfo.empty()) {
        GTEST_SKIP() << "VELOX_TEST_PG_CONNINFO not set -- skipping (no Postgres fixture)";
    }

    RawConn fixture(conninfo);
    ASSERT_TRUE(fixture.ok()) << "could not connect with VELOX_TEST_PG_CONNINFO";
    ASSERT_TRUE(fixture.exec(readFile(VELOX_AUDIT_SQL_INIT)));
    ASSERT_TRUE(fixture.exec("DELETE FROM velox_audit.order_event WHERE shard_id = " +
                             std::to_string(kTestShardId)));
    ASSERT_TRUE(fixture.exec("DELETE FROM velox_audit.trade WHERE shard_id = " +
                             std::to_string(kTestShardId)));
    ASSERT_TRUE(fixture.exec("DELETE FROM velox_audit.ingest_checkpoint WHERE shard_id = " +
                             std::to_string(kTestShardId)));

    PgWriter writer(conninfo);
    const std::vector<OrderEventRow> batch = makeOrderEvents(1, 5);
    const sequencer::Checkpoint cp{.createdCounter = 0, .offset = 100, .lastGoodSeq = 5};

    ASSERT_TRUE(writer.writeBatch(kTestShardId, batch, {}, cp));
    EXPECT_EQ(fixture.countRows("velox_audit.order_event", kTestShardId), 5);

    // The at-least-once retry a real velox_auditd would perform after a network blip on the ACK
    // of an already-committed batch: same rows, same checkpoint, sent again.
    ASSERT_TRUE(writer.writeBatch(kTestShardId, batch, {}, cp));
    EXPECT_EQ(fixture.countRows("velox_audit.order_event", kTestShardId), 5)
        << "retrying an already-committed batch must not duplicate rows";

    sequencer::Checkpoint resumed{};
    ASSERT_TRUE(writer.loadCheckpoint(kTestShardId, resumed));
    EXPECT_EQ(resumed.lastGoodSeq, 5);
}

TEST(IdempotentIngest, AbortedTransactionLeavesNoPartialRowsAndRestartIsClean) {
    const std::string conninfo = requireConninfo();
    if (conninfo.empty()) {
        GTEST_SKIP() << "VELOX_TEST_PG_CONNINFO not set -- skipping (no Postgres fixture)";
    }

    RawConn fixture(conninfo);
    ASSERT_TRUE(fixture.ok());
    ASSERT_TRUE(fixture.exec(readFile(VELOX_AUDIT_SQL_INIT)));
    ASSERT_TRUE(fixture.exec("DELETE FROM velox_audit.order_event WHERE shard_id = " +
                             std::to_string(kTestShardId)));
    ASSERT_TRUE(fixture.exec("DELETE FROM velox_audit.trade WHERE shard_id = " +
                             std::to_string(kTestShardId)));
    ASSERT_TRUE(fixture.exec("DELETE FROM velox_audit.ingest_checkpoint WHERE shard_id = " +
                             std::to_string(kTestShardId)));

    // Batch A: ingested for real, commits.
    PgWriter writer(conninfo);
    const std::vector<OrderEventRow> batchA = makeOrderEvents(1, 3);
    const sequencer::Checkpoint cpA{.createdCounter = 0, .offset = 60, .lastGoodSeq = 3};
    ASSERT_TRUE(writer.writeBatch(kTestShardId, batchA, {}, cpA));
    ASSERT_EQ(fixture.countRows("velox_audit.order_event", kTestShardId), 3);

    // Batch B: simulate a crash mid-transaction -- BEGIN, INSERT some rows matching what batch B
    // would have written, then drop the connection without COMMIT. Postgres rolls back on
    // connection loss; nothing from this should be visible afterward.
    {
        RawConn crashing(conninfo);
        ASSERT_TRUE(crashing.ok());
        ASSERT_TRUE(crashing.exec("BEGIN"));
        ASSERT_TRUE(crashing.exec(
            "INSERT INTO velox_audit.order_event (shard_id, global_seq, kind, order_id, "
            "participant, side, order_type, price, quantity) VALUES (" +
            std::to_string(kTestShardId) + ", 4, 0, 1004, 1, 0, 0, 1000000, 10)"));
        // No COMMIT -- RawConn's destructor closes the connection here, which Postgres treats
        // as an abort of the open transaction.
    }
    EXPECT_EQ(fixture.countRows("velox_audit.order_event", kTestShardId), 3)
        << "a connection dropped before COMMIT must leave zero partial rows";

    sequencer::Checkpoint resumeCp{};
    ASSERT_TRUE(writer.loadCheckpoint(kTestShardId, resumeCp));
    EXPECT_EQ(resumeCp.lastGoodSeq, 3) << "checkpoint must still reflect only the committed batch";

    // Real restart-from-checkpoint: batch B is ingested for real this time.
    const std::vector<OrderEventRow> batchB = makeOrderEvents(4, 2);  // seq 4, 5
    const sequencer::Checkpoint cpB{.createdCounter = 0, .offset = 100, .lastGoodSeq = 5};
    ASSERT_TRUE(writer.writeBatch(kTestShardId, batchB, {}, cpB));
    EXPECT_EQ(fixture.countRows("velox_audit.order_event", kTestShardId), 5);

    // Re-run the WHOLE ingest from seq 0 (as a from-scratch velox_auditd restart would) --
    // idempotence, directly: row counts must not move.
    ASSERT_TRUE(writer.writeBatch(kTestShardId, batchA, {}, cpA));
    ASSERT_TRUE(writer.writeBatch(kTestShardId, batchB, {}, cpB));
    EXPECT_EQ(fixture.countRows("velox_audit.order_event", kTestShardId), 5)
        << "replaying the entire ingest from seq 0 must not create duplicates";
}
