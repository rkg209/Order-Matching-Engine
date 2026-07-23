// Spec 007: spawns the REAL velox_gateway binary (not an in-process fake), drives it with a
// real socket client, kills it, restarts it against the same journal directory, and asserts the
// recovered sequence accounts for exactly the orders that were acked before the kill -- the same
// pattern tests/recovery/recover_sigkill_test.cpp uses for velox_live.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include "tests/gateway/gateway_test_harness.hpp"

using namespace velox;
using namespace velox::gateway::test;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("velox_gw_e2e_" + name);
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

// Spawns `velox_gateway` with the given args; returns its pid and a FILE* over its stderr (where
// the "GATEWAY listening ... recovered_seq=N" line is printed).
struct SpawnedGateway {
    pid_t pid = -1;
    FILE* errFile = nullptr;
};

SpawnedGateway spawnGateway(const std::string& binPath, const fs::path& journalDir,
                            unsigned short port, const fs::path& creds) {
    int errPipe[2];
    if (pipe(errPipe) != 0) {
        ADD_FAILURE() << "pipe() failed";
        return {};
    }
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(errPipe[1], STDERR_FILENO);
        close(errPipe[0]);
        close(errPipe[1]);
        const std::string journalArg = "--journal=" + journalDir.string();
        const std::string portArg = "--port=" + std::to_string(port);
        const std::string credsArg = "--creds=" + creds.string();
        execl(binPath.c_str(), binPath.c_str(), journalArg.c_str(), portArg.c_str(),
              credsArg.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    if (pid <= 0) {
        ADD_FAILURE() << "fork() failed";
        return {};
    }
    close(errPipe[1]);
    SpawnedGateway sg;
    sg.pid = pid;
    sg.errFile = fdopen(errPipe[0], "r");
    return sg;
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

// Parses "recovered_seq=N" out of whatever text has appeared on the pipe so far.
Seq parseRecoveredSeq(FILE* errFile, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::string buffer;
    char c;
    while (std::chrono::steady_clock::now() < deadline) {
        int ch = std::fgetc(errFile);
        if (ch == EOF) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        c = static_cast<char>(ch);
        buffer.push_back(c);
        auto pos = buffer.find("recovered_seq=");
        if (pos != std::string::npos) {
            // Keep reading digits until a non-digit terminates the number -- "=" can land in
            // the buffer before any of the digits that follow it have actually arrived yet.
            std::string digits;
            while (std::chrono::steady_clock::now() < deadline) {
                const int next = std::fgetc(errFile);
                if (next == EOF) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                const char dc = static_cast<char>(next);
                if (std::isdigit(static_cast<unsigned char>(dc))) {
                    digits.push_back(dc);
                } else if (!digits.empty()) {
                    break;
                }
            }
            return digits.empty() ? -1 : std::stoll(digits);
        }
    }
    return -1;
}

}  // namespace

TEST(GatewayE2E, KillThenRestartRecoversExactSequence) {
#ifndef VELOX_GATEWAY_BIN
    GTEST_SKIP() << "VELOX_GATEWAY_BIN not defined";
#else
    const std::string binPath = VELOX_GATEWAY_BIN;
    ASSERT_TRUE(fs::exists(binPath)) << "velox_gateway binary not found at " << binPath;

    const fs::path journalDir = tempDir("kill_restart");
    const fs::path credsFile = journalDir / "creds.txt";
    unsigned char token[32];
    makeToken(0x77, token);
    writeCredsFile(credsFile, /*participant=*/1, token);

    const unsigned short port = 19847;

    SpawnedGateway sg = spawnGateway(binPath, journalDir, port, credsFile);
    ASSERT_GT(sg.pid, 0);
    ASSERT_TRUE(waitForPortOpen(port, std::chrono::seconds(5)))
        << "gateway never opened its listening port";

    constexpr int kOrders = 20;
    {
        TestClient client(port);
        ASSERT_TRUE(client.login(1, token));
        for (int i = 0; i < kOrders; ++i) {
            // Non-crossing orders (all BUYs at distinct prices): each produces exactly one
            // NEW_ACK, so counting acks tells us exactly how many are durable.
            client.sendNewOrder(static_cast<std::uint64_t>(i + 2), i + 1, Side::Buy,
                                (100 + i) * kPriceScale, 10);
            protocol::DecodedMessage m;
            ASSERT_TRUE(client.readOne(m));
            if (m.type == protocol::MessageType::Reject) {
                FAIL() << "order " << (i + 1)
                       << " rejected, reason=" << static_cast<int>(m.reject.reason);
            }
            ASSERT_EQ(m.type, protocol::MessageType::ExecReport);
            EXPECT_EQ(m.execReport.execType, protocol::ExecType::NewAck);
        }
        client.close();
    }

    kill(sg.pid, SIGKILL);
    int status = 0;
    waitpid(sg.pid, &status, 0);
    if (sg.errFile) std::fclose(sg.errFile);

    SpawnedGateway restarted = spawnGateway(binPath, journalDir, port, credsFile);
    ASSERT_GT(restarted.pid, 0);
    ASSERT_TRUE(waitForPortOpen(port, std::chrono::seconds(5)))
        << "restarted gateway never opened its listening port";

    const Seq recoveredSeq = parseRecoveredSeq(restarted.errFile, std::chrono::seconds(5));
    EXPECT_EQ(recoveredSeq, kOrders)
        << "every order that was NEW_ACKed before the kill must be durable -- NEW_ACK is sent "
           "only after Sequencer::trySubmit() durably journals the command";

    kill(restarted.pid, SIGKILL);
    waitpid(restarted.pid, &status, 0);
    if (restarted.errFile) std::fclose(restarted.errFile);
#endif
}
