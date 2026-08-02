// Spec 010 T7.3 / DoD: "the visualizer sends zero bytes toward the engine, verified at the
// socket level." A stub TCP acceptor stands in for the upstream market-data/stats server; it
// pushes real frames downstream (so the client has every opportunity to reply, ack, or otherwise
// write something back) and independently counts every byte it ever reads FROM the accepted
// socket. The assertion is on that counter, not on reading viz/md_client.hpp's source -- exactly
// what the DoD asks for.

#include <gtest/gtest.h>

#include <asio.hpp>
#include <atomic>
#include <chrono>

#include "marketdata/feed_encoder.hpp"
#include "marketdata/feed_messages.hpp"
#include "viz/md_client.hpp"
#include "viz/stats_client.hpp"

using namespace velox;

namespace {

// Accepts exactly one connection, sends a few real market-data frames to it on a timer, and
// counts every byte it reads back. Runs on the caller's io_context.
class CountingStub {
 public:
    explicit CountingStub(asio::io_context& io) : io_(io), acceptor_(io), timer_(io) {}

    unsigned short listen() {
        asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), 0);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();
        doAccept();
        return acceptor_.local_endpoint().port();
    }

    std::size_t bytesReceivedFromClient() const noexcept {
        return bytesFromClient_.load(std::memory_order_relaxed);
    }

 private:
    void doAccept() {
        acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (ec) return;
            socket_ = std::move(socket);
            connected_ = true;
            doRead();
            sendFrames(0);
        });
    }

    void doRead() {
        socket_.async_read_some(asio::buffer(readBuf_), [this](std::error_code ec, std::size_t n) {
            if (ec) return;
            bytesFromClient_.fetch_add(n, std::memory_order_relaxed);
            doRead();
        });
    }

    // Pushes a handful of real L2Delta frames, mimicking a live feed, so the client has real
    // traffic to (mis)behave in response to.
    void sendFrames(int i) {
        if (i >= 10 || !connected_) return;
        std::byte buf[protocol::kFrameHeaderSize + protocol::kMsgTypeSize + protocol::kMaxFrame];
        const marketdata::L2DeltaMsg m{1,
                                       static_cast<std::uint64_t>(i + 1),
                                       Side::Buy,
                                       protocol::L2Action::Add,
                                       (100 + i) * kPriceScale,
                                       10,
                                       1};
        const std::size_t n = marketdata::encodeL2Delta(m, buf);
        auto outBuf = std::make_shared<std::vector<std::byte>>(buf, buf + n);
        asio::async_write(socket_, asio::buffer(*outBuf),
                          [outBuf](std::error_code, std::size_t) {});
        timer_.expires_after(std::chrono::milliseconds(20));
        timer_.async_wait([this, i](std::error_code ec) {
            if (!ec) sendFrames(i + 1);
        });
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    asio::ip::tcp::socket socket_{io_};
    asio::steady_timer timer_;
    std::array<std::byte, 4096> readBuf_{};
    std::atomic<std::size_t> bytesFromClient_{0};
    bool connected_ = false;
};

}  // namespace

TEST(ReadOnly, MdClientNeverWritesToUpstream) {
    asio::io_context io;
    CountingStub stub(io);
    const unsigned short port = stub.listen();

    viz::MdClient client(io, "127.0.0.1", std::to_string(port));
    client.start();

    io.run_for(std::chrono::milliseconds(500));

    EXPECT_TRUE(client.connected());
    EXPECT_EQ(stub.bytesReceivedFromClient(), 0u);
}

TEST(ReadOnly, StatsClientNeverWritesToUpstream) {
    asio::io_context io;
    CountingStub stub(io);
    const unsigned short port = stub.listen();

    viz::StatsClient client(io, "127.0.0.1", std::to_string(port));
    client.start();

    io.run_for(std::chrono::milliseconds(500));

    EXPECT_EQ(stub.bytesReceivedFromClient(), 0u);
}
