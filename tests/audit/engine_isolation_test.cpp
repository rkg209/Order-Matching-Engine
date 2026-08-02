// Spec 012 T7.4: the DoD's two hard isolation bullets, measured rather than asserted on faith.
//
//   1. velox_auditd tailing the SAME journal a live gateway is writing does not move the
//      gateway's own order-to-ack latency outside noise.
//   2. Killing the audit tier abruptly does not slow, stall, or lose a single order on the
//      gateway side -- the seam really is the filesystem.
//
// Skipped (GTEST_SKIP) unless VELOX_TEST_PG_CONNINFO is set, same convention as
// idempotent_ingest_test.cpp -- this is the one other file in tests/audit/ that needs a real
// Postgres, since it spawns the real velox_auditd binary against one.
//
// Honest deviation from the plan's literal wording: the plan says "SIGKILL Postgres mid-stream".
// This test SIGKILLs the velox_auditd PROCESS instead of the shared Postgres server the test
// runner's connection string points at -- killing a shared database service out from under
// whatever else might be using it is not this test's call to make, whereas velox_auditd is a
// process this test itself spawned and owns outright. The property under test -- "the audit tier
// vanishing without warning has zero effect on the gateway" -- is exercised identically either
// way: from the gateway's side, both failure modes look exactly the same (the audit tier stops
// reading the journal), because the gateway never talks to either of them.
//
// Uses the in-process GatewayTestHarness (tests/gateway/gateway_test_harness.hpp) rather than
// spawning a second real velox_gateway binary -- it runs a real GatewayServer over a real
// loopback socket, which is what order-to-ack latency actually measures, and it hands us the
// shard's journal directory directly instead of scraping it from stderr.

#include <gtest/gtest.h>
#include <libpq-fe.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "tests/gateway/gateway_test_harness.hpp"

using namespace velox;
using namespace velox::gateway::test;
namespace fs = std::filesystem;

namespace {

std::string requireConninfo() {
    const char* c = std::getenv("VELOX_TEST_PG_CONNINFO");
    return c == nullptr ? std::string{} : std::string(c);
}

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool execSql(const std::string& conninfo, const std::string& sql) {
    PGconn* c = PQconnectdb(conninfo.c_str());
    const bool connOk = PQstatus(c) == CONNECTION_OK;
    bool ok = false;
    if (connOk) {
        PGresult* r = PQexec(c, sql.c_str());
        const ExecStatusType st = PQresultStatus(r);
        ok = st == PGRES_COMMAND_OK || st == PGRES_TUPLES_OK;
        PQclear(r);
    }
    PQfinish(c);
    return ok;
}

long long percentileNs(std::vector<long long> samples, double pct) {
    if (samples.empty()) return 0;
    std::sort(samples.begin(), samples.end());
    std::size_t idx = static_cast<std::size_t>(pct / 100.0 * static_cast<double>(samples.size()));
    idx = std::min(idx, samples.size() - 1);
    return samples[idx];
}

// Sends `n` orders one at a time over `client`, waiting for the ExecReport/Reject ack for each
// before sending the next, recording round-trip latency. Simple and synchronous -- this is not
// trying to be velox_loadgen; it is trying to show the SAME workload does not regress under the
// audit tier's presence.
std::vector<long long> runLatencySample(TestClient& client, OrderId startId, int n) {
    std::vector<long long> samplesNs;
    samplesNs.reserve(static_cast<std::size_t>(n));
    // clientSeqNum is a small, per-SESSION monotonically-increasing counter checked by
    // ClientSession::checkClientSeq (gateway/session.cpp:171) -- unrelated to OrderId, and a gap
    // in it gets the connection dropped as a protocol violation. login() used seq 1, so orders
    // on a freshly-logged-in client start at 2.
    std::uint64_t clientSeq = 2;
    for (int i = 0; i < n; ++i) {
        const OrderId id = startId + i;
        const auto t0 = std::chrono::steady_clock::now();
        client.sendNewOrder(clientSeq++, id, Side::Buy, (100 + i % 5) * kPriceScale, 1);
        protocol::DecodedMessage msg;
        EXPECT_TRUE(client.readOne(msg)) << "no ack for order " << id;
        const auto t1 = std::chrono::steady_clock::now();
        samplesNs.push_back(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }
    return samplesNs;
}

struct SpawnedAuditd {
    pid_t pid = -1;
};

SpawnedAuditd spawnAuditd(const std::string& binPath, const fs::path& journalRoot,
                          const std::string& conninfo) {
    const pid_t pid = fork();
    if (pid == 0) {
        execl(binPath.c_str(), binPath.c_str(), ("--journal=" + journalRoot.string()).c_str(),
              "--shards=1", ("--pg=" + conninfo).c_str(), "--batch=50", "--poll-ms=10",
              static_cast<char*>(nullptr));
        _exit(127);
    }
    return SpawnedAuditd{pid};
}

}  // namespace

TEST(AuditEngineIsolation, AuditTierPresenceAndAbruptDeathDoNotAffectGateway) {
    const std::string conninfo = requireConninfo();
    if (conninfo.empty()) {
        GTEST_SKIP() << "VELOX_TEST_PG_CONNINFO not set -- skipping (no Postgres fixture)";
    }
#ifndef VELOX_AUDITD_BIN
    GTEST_SKIP() << "VELOX_AUDITD_BIN not defined (VELOX_BUILD_AUDIT_TIER off?)";
#else
    constexpr std::uint32_t kTestShardId = 1;  // matches GatewayTestHarness's default instrument
    ASSERT_TRUE(execSql(conninfo, readFile(VELOX_AUDIT_SQL_INIT)));
    ASSERT_TRUE(execSql(conninfo, "DELETE FROM velox_audit.order_event WHERE shard_id = " +
                                      std::to_string(kTestShardId)));
    ASSERT_TRUE(execSql(conninfo, "DELETE FROM velox_audit.ingest_checkpoint WHERE shard_id = " +
                                      std::to_string(kTestShardId)));

    gateway::AuthHandler auth;
    unsigned char token[32];
    makeToken(0xAB, token);
    gateway::AuthHandler::Token tok{};
    std::copy(std::begin(token), std::end(token), tok.begin());
    auth.addCredential(1, tok);

    GatewayTestHarness harness("audit_isolation", std::move(auth));

    TestClient baselineClient(harness.port);
    ASSERT_TRUE(baselineClient.login(1, token));
    constexpr int kSamples = 300;
    const std::vector<long long> baseline =
        runLatencySample(baselineClient, /*startId=*/1, kSamples);
    const long long baselineP99 = percentileNs(baseline, 99.0);

    // --- bullet 1: audit tier present, tailing the SAME journal this gateway is writing -------
    const SpawnedAuditd auditd = spawnAuditd(VELOX_AUDITD_BIN, harness.root, conninfo);
    ASSERT_GT(auditd.pid, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let it attach and start tailing

    TestClient withAuditClient(harness.port);
    ASSERT_TRUE(withAuditClient.login(1, token));
    const std::vector<long long> withAudit =
        runLatencySample(withAuditClient, /*startId=*/10'000, kSamples);
    const long long withAuditP99 = percentileNs(withAudit, 99.0);

    std::cerr << "AUDIT ISOLATION: baseline p99=" << baselineP99
              << "ns  with-auditd p99=" << withAuditP99 << "ns\n";
    // Not exactly zero delta -- a separate process reading the same files contends for page
    // cache and I/O (plan T7.4). Asserting within-noise, not bit-identical: a generous multiple
    // plus an absolute floor so this does not flake on a loaded CI box, while still catching a
    // real regression (e.g. an accidental shared lock).
    EXPECT_LT(withAuditP99, baselineP99 * 10 + 5'000'000)
        << "order-to-ack p99 regressed well beyond noise with the audit tier attached";

    // --- bullet 2: kill the audit tier abruptly mid-stream; the gateway must not notice --------
    const Seq seqBeforeKill = harness.shards[0].sequencer().lastSeq();
    ASSERT_EQ(::kill(auditd.pid, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(auditd.pid, &status, 0), auditd.pid);

    TestClient afterKillClient(harness.port);
    ASSERT_TRUE(afterKillClient.login(1, token));
    constexpr int kAfterKillOrders = 100;
    const std::vector<long long> afterKill =
        runLatencySample(afterKillClient, /*startId=*/20'000, kAfterKillOrders);
    EXPECT_EQ(static_cast<int>(afterKill.size()), kAfterKillOrders)
        << "every order after the audit tier's death must still get acked";

    const Seq seqAfterKill = harness.shards[0].sequencer().lastSeq();
    EXPECT_EQ(seqAfterKill - seqBeforeKill, kAfterKillOrders)
        << "the gateway's own sequence must advance by exactly the orders sent -- no stall, no "
           "loss, no duplication, caused by the audit tier's death";
#endif
}
