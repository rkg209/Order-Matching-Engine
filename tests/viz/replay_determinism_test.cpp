// Spec 010 T7.4 / DoD: "it runs from a replayed journal, producing an identical demo every
// time." Spawns the real velox_loadgen binary twice with --replay-journal=... over the SAME
// fixture journal, each time capturing the full market-data byte stream a subscriber sees, and
// asserts the two captures are byte-identical. Same fork/execl-a-real-binary pattern as
// tests/recovery/recover_sigkill_test.cpp -- this is not a claim about the code, it is a
// measurement of two real process runs.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "common/types.hpp"
#include "ipc/command.hpp"
#include "sequencer/journal_writer.hpp"

using namespace velox;
namespace fs = std::filesystem;

namespace {

fs::path tempDir(const std::string& name) {
    fs::path p = fs::temp_directory_path() / ("velox_viz_replay_test_" + name);
    std::error_code ec;
    fs::remove_all(p, ec);
    fs::create_directories(p);
    return p;
}

// A fixed, deterministic sequence of New orders -- mostly resting (alternating sides at distinct
// price levels), a couple of crossing trades, and one cancel, so the replayed md stream exercises
// L2Delta/L3Order/TradeTick, not just the resting path.
void writeFixtureJournal(const fs::path& dir) {
    sequencer::JournalWriter journal(dir);
    Seq seq = 0;
    auto newOrder = [&](OrderId id, Side side, Price price, Quantity qty, ParticipantId pid) {
        ipc::Command c{};
        c.id = id;
        c.kind = ipc::CommandKind::New;
        c.side = side;
        c.price = price;
        c.quantity = qty;
        c.participant = pid;
        c.type = OrderType::Limit;
        journal.append(++seq, c.kind, c);
    };
    for (int i = 0; i < 10; ++i) {
        newOrder(1000 + i, Side::Buy, (100 - i) * kPriceScale, 10, 1);
    }
    for (int i = 0; i < 5; ++i) {
        newOrder(2000 + i, Side::Sell, (105 + i) * kPriceScale, 10, 2);
    }
    // Crosses the resting bid at 100.
    newOrder(3000, Side::Sell, 100 * kPriceScale, 15, 3);

    ipc::Command cancel{};
    cancel.id = 1005;
    cancel.kind = ipc::CommandKind::Cancel;
    journal.append(++seq, cancel.kind, cancel);
}

std::string findVeloxLoadgenBin() {
#ifdef VELOX_LOADGEN_BIN
    return VELOX_LOADGEN_BIN;
#else
    return "";
#endif
}

// Connects to 127.0.0.1:port with retries (the child may not have bound its listener the
// instant it forked), then reads until the peer closes the connection (the replay process exits
// once its single pass completes), returning every byte observed.
std::string captureMdStream(unsigned short port, std::chrono::seconds connectTimeout) {
    const auto deadline = std::chrono::steady_clock::now() + connectTimeout;
    int fd = -1;
    while (std::chrono::steady_clock::now() < deadline) {
        fd = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            break;
        }
        ::close(fd);
        fd = -1;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (fd < 0) return "";

    std::string out;
    char buf[8192];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fd);
    return out;
}

// Runs one full replay pass: fork/execl velox_loadgen --replay-journal=..., connect a subscriber,
// capture its byte stream, wait for the child to exit.
std::string runOneReplay(const std::string& bin, const fs::path& journalDir, unsigned short port) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        const std::string journalArg = "--replay-journal=" + journalDir.string();
        const std::string portArg = "--md-port=" + std::to_string(port);
        ::execl(bin.c_str(), bin.c_str(), journalArg.c_str(), portArg.c_str(),
                "--start-on-subscriber", "--rate=max", static_cast<char*>(nullptr));
        ::_exit(127);
    }

    const std::string stream = captureMdStream(port, std::chrono::seconds(10));

    int status = 0;
    ::waitpid(pid, &status, 0);
    return stream;
}

}  // namespace

TEST(ReplayDeterminism, SameJournalProducesByteIdenticalMdStream) {
    const std::string bin = findVeloxLoadgenBin();
    ASSERT_FALSE(bin.empty()) << "VELOX_LOADGEN_BIN not set at compile time";

    const fs::path journalDir = tempDir("journal");
    writeFixtureJournal(journalDir);

    // Same fixed port for both runs, sequentially -- the first process has fully exited (waitpid
    // above) before the second one binds it.
    constexpr unsigned short kPort = 19777;

    const std::string run1 = runOneReplay(bin, journalDir, kPort);
    const std::string run2 = runOneReplay(bin, journalDir, kPort);

    ASSERT_FALSE(run1.empty()) << "first replay run produced no md bytes at all";
    EXPECT_EQ(run1, run2);
}
