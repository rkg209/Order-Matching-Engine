// Spec 013 T7: spawns the REAL velox_gateway binary to produce a real journal, sends it a batch
// of orders, kills it, then spawns the REAL velox_adminctl binary against that same journal
// directory and asserts /api/v1/engine/status's last_good_seq matches the number of orders that
// were durably acked -- same "drive the real binaries" pattern as
// tests/gateway/e2e_test.cpp / tests/recovery/recover_sigkill_test.cpp.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>

#include <asio.hpp>

#include "admin/hmac_sha256.hpp"
#include "common/base64.hpp"
#include "tests/gateway/gateway_test_harness.hpp"

using namespace velox;
using namespace velox::gateway::test;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("velox_admin_e2e_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

void writeCredsFile(const fs::path& path, ParticipantId id, const unsigned char token[32]) {
    std::ofstream f(path);
    f << id << " ";
    static const char* hexDigits = "0123456789abcdef";
    for (int i = 0; i < 32; ++i) {
        f << hexDigits[(token[i] >> 4) & 0xF] << hexDigits[token[i] & 0xF];
    }
    f << "\n";
}

pid_t spawnProc(const std::string& binPath, const std::vector<std::string>& args,
                const std::vector<std::pair<std::string, std::string>>& env = {}) {
    const pid_t pid = fork();
    if (pid == 0) {
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(binPath.c_str()));
        for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        for (const auto& [k, v] : env) {
            setenv(k.c_str(), v.c_str(), 1);
        }
        execv(binPath.c_str(), argv.data());
        _exit(127);
    }
    return pid;
}

bool waitForPortOpen(unsigned short port, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        try {
            asio::io_context io;
            asio::ip::tcp::socket sock(io);
            asio::ip::tcp::resolver resolver(io);
            asio::connect(sock, resolver.resolve("127.0.0.1", std::to_string(port)));
            return true;
        } catch (...) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    return false;
}

std::string b64url(const std::string& s) {
    return common::base64Encode(s, /*urlSafe=*/true, /*pad=*/false);
}

std::string mintToken(const std::string& secret) {
    const std::string header = R"({"alg":"HS256","typ":"JWT"})";
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    const std::string payload =
        "{\"sub\":\"e2e\",\"roles\":[\"ADMIN\"],\"iat\":" + std::to_string(now) +
        ",\"exp\":" + std::to_string(now + 600) + ",\"jti\":\"e2e-1\"}";
    const std::string signingInput = b64url(header) + "." + b64url(payload);
    const admin::Sha256Digest sig = admin::hmacSha256(secret, signingInput);
    return signingInput + "." +
           common::base64Encode(sig.data(), sig.size(), /*urlSafe=*/true, /*pad=*/false);
}

struct HttpResult {
    int status = 0;
    std::string body;
};

HttpResult httpGet(unsigned short port, const std::string& target, const std::string& token) {
    asio::io_context io;
    asio::ip::tcp::socket sock(io);
    asio::ip::tcp::resolver resolver(io);
    asio::connect(sock, resolver.resolve("127.0.0.1", std::to_string(port)));

    std::ostringstream req;
    req << "GET " << target << " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        << "Authorization: Bearer " << token << "\r\nConnection: close\r\n\r\n";
    const std::string reqStr = req.str();
    asio::write(sock, asio::buffer(reqStr));

    asio::streambuf buf;
    std::error_code ec;
    asio::read(sock, buf, ec);
    std::istream is(&buf);
    std::string statusLine;
    std::getline(is, statusLine);
    HttpResult result;
    std::istringstream ss(statusLine);
    std::string version;
    ss >> version >> result.status;
    std::string line;
    while (std::getline(is, line)) {
        if (line == "\r" || line.empty()) break;
    }
    std::ostringstream body;
    body << is.rdbuf();
    result.body = body.str();
    return result;
}

}  // namespace

TEST(AdminE2E, EngineStatusMatchesGatewayRecoveredSeq) {
#if !defined(VELOX_GATEWAY_BIN) || !defined(VELOX_ADMINCTL_BIN)
    GTEST_SKIP() << "VELOX_GATEWAY_BIN or VELOX_ADMINCTL_BIN not defined";
#else
    const std::string gatewayBin = VELOX_GATEWAY_BIN;
    const std::string adminctlBin = VELOX_ADMINCTL_BIN;
    ASSERT_TRUE(fs::exists(gatewayBin));
    ASSERT_TRUE(fs::exists(adminctlBin));

    const fs::path journalDir = tempDir("basic");
    const fs::path credsFile = journalDir / "creds.txt";
    unsigned char token[32];
    makeToken(0x55, token);
    writeCredsFile(credsFile, /*participant=*/1, token);

    const unsigned short gwPort = 19901;
    const unsigned short adminPort = 19902;

    const pid_t gwPid = spawnProc(
        gatewayBin, {"--journal=" + journalDir.string(), "--port=" + std::to_string(gwPort),
                     "--creds=" + credsFile.string()});
    ASSERT_GT(gwPid, 0);
    ASSERT_TRUE(waitForPortOpen(gwPort, std::chrono::seconds(5)));

    constexpr int kOrders = 7;
    {
        TestClient client(gwPort);
        ASSERT_TRUE(client.login(1, token));
        for (int i = 0; i < kOrders; ++i) {
            client.sendNewOrder(static_cast<std::uint64_t>(i + 2), i + 1, Side::Buy,
                                (100 + i) * kPriceScale, 10);
            protocol::DecodedMessage m;
            ASSERT_TRUE(client.readOne(m));
            ASSERT_EQ(m.type, protocol::MessageType::ExecReport);
        }
        client.close();
    }

    kill(gwPid, SIGKILL);
    int status = 0;
    waitpid(gwPid, &status, 0);

    const std::string secret = "e2e-admin-secret";
    const pid_t adminPid = spawnProc(
        adminctlBin, {"--journal=" + journalDir.string(), "--port=" + std::to_string(adminPort)},
        {{"VELOX_ADMIN_JWT_SECRET", secret}});
    ASSERT_GT(adminPid, 0);
    ASSERT_TRUE(waitForPortOpen(adminPort, std::chrono::seconds(5)));

    const std::string tok = mintToken(secret);
    const HttpResult r = httpGet(adminPort, "/api/v1/engine/status", tok);
    ASSERT_EQ(r.status, 200);
    EXPECT_NE(r.body.find("\"last_good_seq\":" + std::to_string(kOrders)), std::string::npos)
        << r.body;
    EXPECT_NE(r.body.find("\"source\":\"disk\""), std::string::npos);

    kill(adminPid, SIGKILL);
    waitpid(adminPid, &status, 0);
#endif
}
