#pragma once

// Shared in-process gateway fixture + a minimal synchronous test client, used by session_test,
// routing_test, and backpressure_test. Runs a real GatewayServer against a real (loopback) TCP
// socket in the same process -- lighter than spawning the actual binary (that is what
// e2e_test.cpp is for), but still exercising the real asio session state machine end to end.

#include <array>
#include <asio.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>

#include "engine/order_book.hpp"
#include "gateway/auth.hpp"
#include "gateway/gateway.hpp"
#include "ipc/command.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"
#include "protocol/messages.hpp"
#include "runtime/matching_thread.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/sequencer.hpp"

namespace velox::gateway::test {

inline std::filesystem::path makeTempDir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("velox_gw_test_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

inline BookConfig testConfig() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 10000 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1u << 20;
    return cfg;
}

inline void makeToken(unsigned char byteValue, unsigned char out[32]) {
    for (int i = 0; i < 32; ++i) out[i] = byteValue;
}

// Spins up a full gateway (matching thread + journal + GatewayServer) bound to an OS-assigned
// loopback port, running its io_context on a background thread. Torn down in the destructor.
struct GatewayTestHarness {
    std::filesystem::path journalDir;
    ipc::SpscRing<ipc::Command> inRing;
    runtime::MatchingThread<>::OutRing outRing;
    runtime::MatchingThread<> matching;
    sequencer::JournalWriter journal;
    sequencer::Sequencer<ipc::SpscRing<ipc::Command>> seqr;
    asio::io_context io;
    AuthHandler auth;
    GatewayServer server;
    std::thread ioThread;
    unsigned short port = 0;

    explicit GatewayTestHarness(const std::string& name, AuthHandler authIn = AuthHandler())
        : journalDir(makeTempDir(name)),
          matching(inRing, outRing, testConfig()),
          journal(journalDir),
          seqr(journal, inRing, 0),
          auth(std::move(authIn)),
          server(io, seqr, outRing, auth, 1, testConfig().minPrice, testConfig().maxPrice) {
        matching.start();
        server.listen(0);
        port = server.localPort();
        server.startRouter();
        ioThread = std::thread([this] { io.run(); });
    }

    ~GatewayTestHarness() {
        io.stop();
        if (ioThread.joinable()) ioThread.join();
        server.stopRouter();
        matching.stop();
    }
};

// A minimal synchronous test client: connect, login, send/receive frames, blocking with a
// generous timeout so a protocol bug hangs the test instead of the process.
class TestClient {
 public:
    explicit TestClient(unsigned short port)
        : socket_(io_), decoder_(1, 1 * kPriceScale, 10000 * kPriceScale) {
        asio::ip::tcp::resolver resolver(io_);
        asio::connect(socket_, resolver.resolve("127.0.0.1", std::to_string(port)));
    }

    bool login(ParticipantId participant, const unsigned char token[32],
               std::uint64_t clientSeqNum = 1) {
        protocol::LoginMsg m{};
        m.participantId = participant;
        std::memcpy(m.token, token, 32);
        m.clientSeqNum = clientSeqNum;
        std::byte buf[128];
        const std::size_t n = protocol::encodeLogin(m, buf);
        asio::write(socket_, asio::buffer(buf, n));

        protocol::DecodedMessage msg;
        if (!readOne(msg)) return false;
        return msg.type == protocol::MessageType::LoginAck;
    }

    void sendNewOrder(std::uint64_t clientSeq, OrderId id, Side side, Price price, Quantity qty) {
        protocol::NewOrderMsg m{};
        m.clientSeqNum = clientSeq;
        m.orderId = id;
        m.instrumentId = 1;
        m.side = side;
        m.orderType = protocol::WireOrderType::Limit;
        m.price = price;
        m.quantity = qty;
        m.timeInForce = protocol::WireTimeInForce::Day;
        std::byte buf[128];
        const std::size_t n = protocol::encodeNewOrder(m, buf);
        asio::write(socket_, asio::buffer(buf, n));
    }

    void sendRaw(const std::byte* data, std::size_t n) {
        asio::write(socket_, asio::buffer(data, n));
    }

    // Reads the next decoded message, blocking (with a wall-clock timeout) until one arrives or
    // the connection closes. Returns false on close/error/timeout.
    bool readOne(protocol::DecodedMessage& out) {
        protocol::RejectReason reason;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            const auto r = decoder_.next(out, reason);
            if (r == protocol::FrameDecoder::Result::Ok) return true;
            if (r == protocol::FrameDecoder::Result::Invalid) return false;
            if (std::chrono::steady_clock::now() > deadline) return false;

            std::error_code ec;
            socket_.non_blocking(false);
            socket_.wait(asio::ip::tcp::socket::wait_read, ec);
            if (ec) return false;
            std::array<std::byte, 256> buf{};
            const std::size_t got = socket_.read_some(asio::buffer(buf), ec);
            if (ec || got == 0) return false;
            if (!decoder_.feed(buf.data(), got)) return false;
        }
    }

    void close() {
        std::error_code ec;
        socket_.close(ec);
    }

 private:
    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    protocol::FrameDecoder decoder_;
};

}  // namespace velox::gateway::test
