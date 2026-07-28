// Spec 008 T8 / DoD bullet 4: a subscriber whose queue overflows is disconnected -- never
// buffered further. Split into two focused, deterministic tests rather than one race between a
// "slow" and a "healthy" subscriber sharing a wall-clock budget: real TCP flow control depends on
// the OS's own receive-buffer size, which on a loopback interface can auto-tune to several MB, so
// racing it directly (send fast, hope one side falls behind) turned out to be extremely flaky in
// practice -- either both subscribers "overflow" (a burst fast enough that no write ever
// completes starves EVERYONE, not just the one that never reads) or neither does (paced sends
// complete faster than any reasonably-sized test volume can fill a real kernel buffer). Testing
// each claim in isolation, with volumes chosen for what each mechanism actually needs, is the
// stable version of the same DoD requirement.

#include <gtest/gtest.h>

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <thread>

#include "marketdata/feed_server.hpp"

using namespace velox;
using namespace velox::marketdata;

namespace {

bool waitUntil(const std::function<bool()>& pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

}  // namespace

// A subscriber that never reads at all is the limit case of "slow": floods it with far more than
// maxQueueBytes in one unpaced burst (server.send() calls back to back, from the test thread, no
// pacing) -- FeedSession's application-level queue must catch this and close the session, since
// it cannot wait for real TCP backpressure that this test has no reliable way to force.
TEST(FeedServer, NeverReadingSubscriberOverflowsAndIsDropped) {
    constexpr std::size_t kMaxQueueBytes = 4096;

    asio::io_context io;
    FeedServer server(io, kMaxQueueBytes);
    server.listen(0);
    const unsigned short port = server.localPort();
    std::thread ioThread([&] { io.run(); });

    asio::io_context clientIo;
    asio::ip::tcp::resolver resolver(clientIo);
    asio::ip::tcp::socket slow(clientIo);
    asio::connect(slow, resolver.resolve("127.0.0.1", std::to_string(port)));

    bool ok = waitUntil([&] { return server.sessionCount() == 1; }, std::chrono::seconds(2));
    EXPECT_TRUE(ok);

    if (ok) {
        std::vector<std::byte> chunk(4096, std::byte{0xAB});
        for (int i = 0; i < 32; ++i) {  // 128 KiB total, 32x the queue bound, one unpaced burst
            server.send(chunk.data(), chunk.size());
        }
        EXPECT_TRUE(waitUntil([&] { return server.sessionCount() == 0; }, std::chrono::seconds(2)))
            << "a subscriber that never reads was never dropped";
    }

    std::error_code ec;
    slow.close(ec);
    io.stop();
    ioThread.join();
}

// The other half of the same DoD requirement: a subscriber that IS actively reading must not be
// disconnected during ordinary operation. Paced, modest volume -- comfortably within what a real
// socket drains between sends -- so this is a test of "FeedServer doesn't spuriously drop good
// connections," not a throughput benchmark.
TEST(FeedServer, ActivelyReadingSubscriberIsNeverDisconnected) {
    constexpr std::size_t kMaxQueueBytes = 4096;

    asio::io_context io;
    FeedServer server(io, kMaxQueueBytes);
    server.listen(0);
    const unsigned short port = server.localPort();
    std::thread ioThread([&] { io.run(); });

    asio::io_context clientIo;
    asio::ip::tcp::resolver resolver(clientIo);
    asio::ip::tcp::socket healthy(clientIo);
    asio::connect(healthy, resolver.resolve("127.0.0.1", std::to_string(port)));

    bool ok = waitUntil([&] { return server.sessionCount() == 1; }, std::chrono::seconds(2));
    EXPECT_TRUE(ok);

    std::atomic<bool> stop{false};
    std::atomic<std::size_t> bytesRead{0};
    std::thread reader([&] {
        std::array<std::byte, 4096> buf{};
        std::error_code ec;
        while (!stop.load(std::memory_order_acquire)) {
            const std::size_t n = healthy.read_some(asio::buffer(buf), ec);
            if (ec) return;
            bytesRead.fetch_add(n, std::memory_order_relaxed);
        }
    });

    if (ok) {
        std::vector<std::byte> chunk(200, std::byte{0xCD});
        constexpr int kSends = 40;  // 8 KiB total, paced -- 2x the queue bound, but drained as it
                                    // goes, so it never actually sits in the queue all at once
        for (int i = 0; i < kSends; ++i) {
            server.send(chunk.data(), chunk.size());
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_TRUE(
            waitUntil([&] { return bytesRead.load(std::memory_order_relaxed) >= 200 * kSends; },
                      std::chrono::seconds(2)));
        EXPECT_EQ(server.sessionCount(), 1u) << "an actively-reading subscriber was disconnected";
    }

    stop.store(true, std::memory_order_release);
    std::error_code ec;
    healthy.close(ec);
    reader.join();

    io.stop();
    ioThread.join();
}
