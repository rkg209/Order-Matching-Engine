// Spec 013 T5: exercises the full HttpServer + AdminStore + JWT wiring in-process (port 0), the
// same pattern tests/viz/readonly_test.cpp uses -- a synthetic journal/snapshot directory built
// directly with JournalWriter/SnapshotWriter stands in for a real velox_gateway run.

#include <gtest/gtest.h>

#include <asio.hpp>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <thread>

#include "admin/hmac_sha256.hpp"
#include "admin/http_server.hpp"
#include "admin/routes.hpp"
#include "common/base64.hpp"
#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/snapshot_writer.hpp"

using namespace velox;
using namespace velox::admin;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("velox_admin_routes_test_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

BookConfig testConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 4096;
    return cfg;
}

ipc::Command newCmd(OrderId id, Side side, Price price, Quantity qty, ParticipantId pid) {
    ipc::Command c{};
    c.id = id;
    c.price = price;
    c.quantity = qty;
    c.participant = pid;
    c.side = side;
    c.kind = ipc::CommandKind::New;
    c.type = OrderType::Limit;
    return c;
}

std::string b64url(const std::string& s) {
    return common::base64Encode(s, /*urlSafe=*/true, /*pad=*/false);
}

std::string signToken(const std::string& payloadJson, const std::string& secret) {
    const std::string header = R"({"alg":"HS256","typ":"JWT"})";
    const std::string signingInput = b64url(header) + "." + b64url(payloadJson);
    const Sha256Digest sig = hmacSha256(secret, signingInput);
    return signingInput + "." + common::base64Encode(sig.data(), sig.size(), true, false);
}

std::string mintToken(const std::string& secret, const std::string& roles = "[\"ADMIN\"]",
                      std::int64_t ttl = 600) {
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string payload = "{\"sub\":\"test\",\"roles\":" + roles +
                                ",\"iat\":" + std::to_string(now) +
                                ",\"exp\":" + std::to_string(now + ttl) + ",\"jti\":\"t-1\"}";
    return signToken(payload, secret);
}

struct HttpResult {
    int status = 0;
    std::string body;
};

HttpResult httpRequest(unsigned short port, const std::string& method, const std::string& target,
                       const std::string& bearerToken = "") {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    asio::ip::tcp::resolver resolver(io);
    asio::connect(sock, resolver.resolve("127.0.0.1", std::to_string(port)));

    std::ostringstream req;
    req << method << " " << target << " HTTP/1.1\r\nHost: 127.0.0.1\r\n";
    if (!bearerToken.empty()) {
        req << "Authorization: Bearer " << bearerToken << "\r\n";
    }
    req << "Connection: close\r\n\r\n";
    const std::string reqStr = req.str();
    asio::write(sock, asio::buffer(reqStr));

    asio::streambuf buf;
    std::error_code ec;
    asio::read(sock, buf, ec);  // reads until EOF (Connection: close)

    std::istream is(&buf);
    std::string statusLine;
    std::getline(is, statusLine);
    HttpResult result;
    {
        std::istringstream ss(statusLine);
        std::string version;
        ss >> version >> result.status;
    }
    std::string line;
    while (std::getline(is, line)) {
        if (line == "\r" || line.empty()) break;
    }
    std::ostringstream body;
    body << is.rdbuf();
    result.body = body.str();
    return result;
}

class RoutesTest : public ::testing::Test {
 protected:
    void SetUp() override {
        root_ = tempDir("basic");
        const fs::path shard = root_ / "shard-1";
        {
            sequencer::JournalWriter w(shard / "journal");
            ASSERT_TRUE(
                w.append(1, ipc::CommandKind::New, newCmd(1, Side::Buy, 100 * kPriceScale, 10, 1)));
            ASSERT_TRUE(
                w.append(2, ipc::CommandKind::New, newCmd(2, Side::Buy, 101 * kPriceScale, 5, 1)));
        }
        {
            sequencer::SnapshotWriter sw(shard / "snapshots");
            const OrderBook book(testConfig());
            ASSERT_TRUE(sw.write(book, 2, testConfig()));
        }

        store_ = std::make_unique<AdminStore>(root_);
        server_ = std::make_unique<HttpServer>(io_);
        registerAdminRoutes(*server_, *store_, secret_, revoked_);
        server_->listen(0, "127.0.0.1");
        port_ = server_->localPort();
        ioThread_ = std::thread([this] {
            auto guard = asio::make_work_guard(io_);
            io_.run();
        });
    }

    void TearDown() override {
        io_.stop();
        ioThread_.join();
    }

    fs::path root_;
    asio::io_context io_;
    std::thread ioThread_;
    std::unique_ptr<AdminStore> store_;
    std::unique_ptr<HttpServer> server_;
    unsigned short port_ = 0;
    std::string secret_ = "routes-test-secret";
    std::unordered_set<std::string> revoked_;
};

}  // namespace

TEST_F(RoutesTest, HealthzUnauthenticated) {
    const auto r = httpRequest(port_, "GET", "/healthz");
    EXPECT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"status\":\"ok\""), std::string::npos);
}

TEST_F(RoutesTest, NoTokenIs401) {
    const auto r = httpRequest(port_, "GET", "/api/v1/engine/status");
    EXPECT_EQ(r.status, 401);
}

TEST_F(RoutesTest, BadSignatureIs401) {
    std::string token = mintToken(secret_);
    token.back() = (token.back() == 'A') ? 'B' : 'A';
    const auto r = httpRequest(port_, "GET", "/api/v1/engine/status", token);
    EXPECT_EQ(r.status, 401);
}

TEST_F(RoutesTest, WrongRoleIs403) {
    const std::string token = mintToken(secret_, "[\"NOT_A_REAL_ROLE\"]");
    const auto r = httpRequest(port_, "GET", "/api/v1/engine/status", token);
    EXPECT_EQ(r.status, 403);
}

TEST_F(RoutesTest, UnknownPathIs404) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/nope", token);
    EXPECT_EQ(r.status, 404);
}

TEST_F(RoutesTest, PostIs405) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "POST", "/api/v1/instruments", token);
    EXPECT_EQ(r.status, 405);
}

TEST_F(RoutesTest, NonNumericInstrumentIdIs400) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/instruments/abc/snapshots", token);
    EXPECT_EQ(r.status, 400);
}

TEST_F(RoutesTest, UnknownInstrumentIdIs404) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/instruments/99/snapshots", token);
    EXPECT_EQ(r.status, 404);
}

TEST_F(RoutesTest, EngineStatusReportsShard) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/engine/status", token);
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"source\":\"disk\""), std::string::npos);
    EXPECT_NE(r.body.find("\"instrument_id\":1"), std::string::npos);
    EXPECT_NE(r.body.find("\"last_good_seq\":2"), std::string::npos);
}

TEST_F(RoutesTest, InstrumentsListsShardOne) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/instruments", token);
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"id\":1"), std::string::npos);
    EXPECT_NE(r.body.find("\"has_more\":false"), std::string::npos);
}

TEST_F(RoutesTest, JournalSegmentsListed) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/instruments/1/journal/segments", token);
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"first_seq\":1"), std::string::npos);
    EXPECT_NE(r.body.find("\"header_valid\":true"), std::string::npos);
}

TEST_F(RoutesTest, SnapshotsListed) {
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/instruments/1/snapshots", token);
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"global_seq\":2"), std::string::npos);
    EXPECT_NE(r.body.find("\"crc_valid\":true"), std::string::npos);
}

TEST_F(RoutesTest, PaginationEnvelopeRespectsLimit) {
    const std::string token = mintToken(secret_);
    const auto r =
        httpRequest(port_, "GET", "/api/v1/instruments/1/journal/segments?limit=0", token);
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"items\":[]"), std::string::npos);
    EXPECT_NE(r.body.find("\"has_more\":true"), std::string::npos);
}

TEST_F(RoutesTest, RevokedJtiIs401) {
    revoked_.insert("t-1");
    const std::string token = mintToken(secret_);
    const auto r = httpRequest(port_, "GET", "/api/v1/engine/status", token);
    EXPECT_EQ(r.status, 401);
}
