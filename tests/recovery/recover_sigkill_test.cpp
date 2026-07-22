// Spec 006 DoD: kill the engine mid-stream with SIGKILL (not a graceful shutdown), restart in
// --mode=recover, and assert byte-identical state -- the recovered digest and trade stream must
// match exactly what was durably ACKed before the kill.
//
// This forks and execs the REAL velox_live binary (not an in-process fake), feeds it commands
// one at a time over a pipe, SIGKILLs it partway through, then runs `--mode=recover` as a
// second real process and compares its reported digest against an in-process reference replay
// of exactly the commands that were ACKed before the kill.

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/scenario.hpp"
#include "engine/order_book.hpp"
#include "recovery/state_digest.hpp"

using namespace velox;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("velox_sigkill_test_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// Mirrors apps/velox_live.cpp's liveConfig() exactly -- recovery only makes sense if this
// process and the child process agree on price range / tick / capacity.
BookConfig liveConfigMirror() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 10000 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1u << 20;
    return cfg;
}

std::vector<std::string> generateScenarioLines(int n) {
    std::vector<std::string> lines;
    lines.reserve(static_cast<std::size_t>(n));
    for (int i = 1; i <= n; ++i) {
        std::ostringstream ss;
        const char* side = (i % 2 == 0) ? "BUY" : "SELL";
        const double price = 100.0 + static_cast<double>(i % 7);
        ss << "NEW " << i << " " << side << " " << price << " " << (1 + i % 5) << " " << (i % 4);
        lines.push_back(ss.str());
    }
    return lines;
}

// Runs a child process, feeding `stdinLines` one at a time and reading matching stdout lines,
// until either all lines are sent or `ackTarget` ACK lines have been observed -- then SIGKILLs
// the child. Returns the lines of stdout actually observed before the kill.
struct KillResult {
    std::vector<std::string> observedStdout;
    pid_t pid = -1;
};

KillResult runAndKill(const std::string& binPath, const std::string& journalDir,
                      const std::vector<std::string>& lines, int ackTarget) {
    int inPipe[2];
    int outPipe[2];
    if (pipe(inPipe) != 0 || pipe(outPipe) != 0) {
        ADD_FAILURE() << "pipe() failed";
        return {};
    }

    const pid_t pid = fork();
    if (pid == 0) {
        // child
        dup2(inPipe[0], STDIN_FILENO);
        dup2(outPipe[1], STDOUT_FILENO);
        close(inPipe[0]);
        close(inPipe[1]);
        close(outPipe[0]);
        close(outPipe[1]);
        const std::string journalArg = "--journal=" + journalDir;
        execl(binPath.c_str(), binPath.c_str(), "--mode=live", journalArg.c_str(),
              "--snapshot-every=50", static_cast<char*>(nullptr));
        _exit(127);
    }

    if (pid <= 0) {
        ADD_FAILURE() << "fork() failed";
        return {};
    }
    close(inPipe[0]);
    close(outPipe[1]);

    FILE* inFile = fdopen(inPipe[1], "w");
    FILE* outFile = fdopen(outPipe[0], "r");
    if (inFile == nullptr || outFile == nullptr) {
        ADD_FAILURE() << "fdopen() failed";
        return {};
    }

    KillResult result;
    result.pid = pid;
    int acksSeen = 0;
    char buf[1024];
    for (const auto& line : lines) {
        std::fprintf(inFile, "%s\n", line.c_str());
        std::fflush(inFile);
        // Read stdout lines produced for this command (TRADE* then exactly one ACK).
        for (;;) {
            if (std::fgets(buf, sizeof(buf), outFile) == nullptr) {
                break;  // child died/closed stdout
            }
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            result.observedStdout.push_back(s);
            if (s.rfind("ACK ", 0) == 0) {
                ++acksSeen;
                break;
            }
        }
        if (acksSeen >= ackTarget) {
            break;
        }
    }

    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);

    std::fclose(inFile);
    std::fclose(outFile);
    return result;
}

// Runs `binPath --mode=recover --journal=journalDir --digest-out=digestFile` to completion and
// returns its stdout.
std::string runRecoverProcess(const std::string& binPath, const std::string& journalDir,
                              const std::string& digestFile) {
    const std::string cmd =
        binPath + " --mode=recover --journal=" + journalDir + " --digest-out=" + digestFile;
    std::string output;
    FILE* p = popen(cmd.c_str(), "r");
    if (p == nullptr) {
        return output;
    }
    char buf[1024];
    while (std::fgets(buf, sizeof(buf), p) != nullptr) {
        output += buf;
    }
    pclose(p);
    return output;
}

recovery::StateDigest parseDigestFile(const fs::path& p) {
    recovery::StateDigest d;
    std::ifstream f(p);
    std::string tok;
    while (f >> tok) {
        auto eq = tok.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = tok.substr(0, eq);
        const std::string val = tok.substr(eq + 1);
        if (key == "lastSeq")
            d.lastSeq = std::stoll(val);
        else if (key == "nextTradeId")
            d.nextTradeId = std::stoll(val);
        else if (key == "restingOrders")
            d.restingOrders = static_cast<std::size_t>(std::stoull(val));
        else if (key == "bodyCrc32")
            d.bodyCrc32 = static_cast<std::uint32_t>(std::stoull(val));
    }
    return d;
}

}  // namespace

TEST(RecoverSigkill, MidStreamKillThenRecoverIsByteIdentical) {
#ifndef VELOX_LIVE_BIN
    GTEST_SKIP() << "VELOX_LIVE_BIN not defined";
#else
    const std::string binPath = VELOX_LIVE_BIN;
    ASSERT_TRUE(fs::exists(binPath)) << "velox_live binary not found at " << binPath;

    const fs::path journalDir = tempDir("midstream");
    const auto lines = generateScenarioLines(400);

    const KillResult killed = runAndKill(binPath, journalDir.string(), lines, /*ackTarget=*/150);

    // Reconstruct exactly which commands were ACKed (i.e. durable) before the kill, and their
    // trade stream, from the child's own stdout -- these are the ONLY commands recovery is
    // allowed to reproduce.
    std::vector<std::string> ackedCommandLines;
    std::vector<std::string> observedTrades;
    std::size_t lineIdx = 0;
    for (const auto& out : killed.observedStdout) {
        if (out.rfind("TRADE ", 0) == 0) {
            observedTrades.push_back(out);
        } else if (out.rfind("ACK ", 0) == 0) {
            ASSERT_LT(lineIdx, lines.size());
            ackedCommandLines.push_back(lines[lineIdx]);
            ++lineIdx;
        }
    }
    ASSERT_GE(ackedCommandLines.size(), 100u)
        << "test did not reach a meaningful number of ACKs before the kill";

    // In-process reference: replay exactly the acked commands directly.
    OrderBook reference(liveConfigMirror());
    Trade storage[64];
    TradeBuffer trades{storage, 64, 0};
    std::vector<std::string> referenceTrades;
    for (const auto& l : ackedCommandLines) {
        common::ScenarioCommand sc;
        ASSERT_TRUE(common::parseScenarioLine(l, sc));
        trades.clear();
        const NewOrder o{sc.id, sc.price, sc.quantity, sc.participant, sc.side, sc.type};
        reference.submit(o, trades);
        for (std::size_t i = 0; i < trades.count; ++i) {
            const Trade& t = trades.data[i];
            std::ostringstream ss;
            ss << "TRADE " << t.id << " agg=" << t.aggressorId << " pass=" << t.passiveId
               << " px=" << t.price << " qty=" << t.quantity;
            referenceTrades.push_back(ss.str());
        }
    }
    const recovery::StateDigest referenceDigest = recovery::computeDigest(reference);

    EXPECT_EQ(observedTrades, referenceTrades)
        << "trade stream from the killed process's own stdout must match a fresh in-process "
           "replay of the same acked commands -- this is exactly what determinism guarantees";

    const fs::path digestFile = journalDir / "recovered_digest.txt";
    const std::string recoverOutput =
        runRecoverProcess(binPath, journalDir.string(), digestFile.string());
    ASSERT_TRUE(fs::exists(digestFile)) << "recover run produced no digest file. stdout was:\n"
                                        << recoverOutput;

    const recovery::StateDigest recoveredDigest = parseDigestFile(digestFile);
    EXPECT_EQ(recoveredDigest, referenceDigest)
        << "recovered state must be byte-identical to the state at the last acked seq";
#endif
}

// A torn tail (killed mid-fsync) must be discarded cleanly -- no crash, no garbage -- and
// recovery must still succeed at whatever the last INTACT record was. This is exercised more
// exhaustively (every byte offset) in recovery_test.cpp's Journal.TornTailAtEveryOffset; this
// test just confirms the same guarantee holds through the real velox_live binary end to end.
TEST(RecoverSigkill, RecoverAfterKillNeverCrashesEvenWithVeryFewAcks) {
#ifndef VELOX_LIVE_BIN
    GTEST_SKIP() << "VELOX_LIVE_BIN not defined";
#else
    const std::string binPath = VELOX_LIVE_BIN;
    ASSERT_TRUE(fs::exists(binPath));

    const fs::path journalDir = tempDir("fewacks");
    const auto lines = generateScenarioLines(20);
    runAndKill(binPath, journalDir.string(), lines, /*ackTarget=*/3);

    const fs::path digestFile = journalDir / "recovered_digest.txt";
    const std::string out = runRecoverProcess(binPath, journalDir.string(), digestFile.string());
    EXPECT_TRUE(fs::exists(digestFile)) << "recover run produced no digest file. stdout was:\n"
                                        << out;
#endif
}
