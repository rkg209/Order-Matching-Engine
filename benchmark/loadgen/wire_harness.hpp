#pragma once

// Spec 009 T2: the `wire` path's fixture. Lifted from tests/gateway/gateway_test_harness.hpp
// (same shape: a real GatewayServer + matching thread + journal bound to an OS-assigned
// loopback port) rather than including a test header from benchmark/ -- this is production-
// adjacent tooling, not a test, and the two should not depend on each other.

#include <unistd.h>

#include <array>
#include <asio.hpp>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>

#include "engine/order_book.hpp"
#include "gateway/auth.hpp"
#include "gateway/gateway.hpp"
#include "ipc/command.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "loadgen/workload.hpp"
#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"
#include "protocol/messages.hpp"
#include "runtime/shard.hpp"

namespace velox::loadgen {

inline std::filesystem::path makeLoadgenTempDir(const std::string& name) {
    std::filesystem::path p = std::filesystem::temp_directory_path() / ("velox_loadgen_" + name);
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    std::filesystem::create_directories(p);
    return p;
}

inline void makeLoadgenToken(unsigned char byteValue, unsigned char out[32]) {
    for (int i = 0; i < 32; ++i) out[i] = byteValue;
}

// GatewayServer takes its AuthHandler BY VALUE (gateway/gateway.hpp) -- it copies whatever it is
// handed at CONSTRUCTION time. Populating credentials in a constructor BODY, after the copy has
// already been made in the member-initializer-list, silently leaves the server's own copy empty
// and every login fails (closing the connection the instant the client tries to send anything).
// So credentials must exist before the AuthHandler is even passed in -- built here, standalone.
inline gateway::AuthHandler makeLoadgenAuth() {
    gateway::AuthHandler auth;
    // The generator logs in as participants 1 (population), 2 and 3 (steady-state rest/cross)
    // -- see loadgen/workload.hpp.
    for (ParticipantId p : {ParticipantId{1}, ParticipantId{2}, ParticipantId{3}}) {
        unsigned char token[32];
        makeLoadgenToken(static_cast<unsigned char>(p), token);
        gateway::AuthHandler::Token tok{};
        std::memcpy(tok.data(), token, 32);
        auth.addCredential(p, tok);
    }
    return auth;
}

// GatewayServer's ctor snapshots the shard set's instrumentSet() once, so the single shard must
// exist before `server` is constructed -- see tests/gateway/gateway_test_harness.hpp's identical
// pattern, which this mirrors.
inline runtime::ShardSet makeLoadgenShardSet(const std::filesystem::path& root) {
    runtime::ShardSet s;
    s.addShard(1, makeConfig(), root, /*cpu=*/0);
    return s;
}

// Spins up a full gateway (one shard: matching thread + journal + GatewayServer) bound to an
// OS-assigned loopback port, running its io_context on a background thread. Torn down in the
// destructor.
struct WireHarness {
    std::filesystem::path journalRoot;
    runtime::ShardSet shards;
    asio::io_context io;
    gateway::AuthHandler auth;
    gateway::GatewayServer server;
    std::thread ioThread;
    unsigned short port = 0;

    WireHarness()
        : journalRoot(makeLoadgenTempDir("bench")),
          shards(makeLoadgenShardSet(journalRoot)),
          auth(makeLoadgenAuth()),
          server(io, shards, auth, makeConfig().minPrice, makeConfig().maxPrice) {
        shards.recoverAndStartAll();
        server.listen(0);
        port = server.localPort();
        server.startRouter();
        ioThread = std::thread([this] { io.run(); });
    }

    ~WireHarness() {
        io.stop();
        if (ioThread.joinable()) ioThread.join();
        server.stopRouter();
        shards.stopAll();
    }
};

// A minimal synchronous socket client: connect, login, send/receive frames.
class LoadgenClient {
 public:
    explicit LoadgenClient(unsigned short port)
        : socket_(io_), decoder_(1, makeConfig().minPrice, makeConfig().maxPrice) {
        asio::ip::tcp::resolver resolver(io_);
        asio::connect(socket_, resolver.resolve("127.0.0.1", std::to_string(port)));
        // See gateway/gateway.hpp's doAccept(): without TCP_NODELAY on BOTH ends, Nagle +
        // delayed ACKs turn this into a several-hundred-Hz drip instead of a latency test.
        std::error_code ndEc;
        socket_.set_option(asio::ip::tcp::no_delay(true), ndEc);
    }

    bool login(ParticipantId participant, const unsigned char token[32]) {
        protocol::LoginMsg m{};
        m.participantId = participant;
        std::memcpy(m.token, token, 32);
        m.clientSeqNum = 1;
        std::byte buf[128];
        const std::size_t n = protocol::encodeLogin(m, buf);
        if (!rawWrite(buf, n)) return false;

        protocol::DecodedMessage msg;
        if (!readOne(msg)) return false;
        return msg.type == protocol::MessageType::LoginAck;
    }

    // Returns false on a write error (e.g. the server closed the connection) instead of
    // throwing -- a benchmark harness must fail loudly with a message, never std::terminate.
    bool sendNewOrder(std::uint64_t clientSeq, OrderId id, Side side, Price price, Quantity qty) {
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
        return rawWrite(buf, n);
    }

    // Blocking read of the next decoded message, via a raw ::read() on the socket's native
    // handle rather than through the asio socket object. The generator thread writes on this
    // same connection concurrently (WirePath submits and observes completions on one socket,
    // since the gateway routes a response only to the session that submitted the order) -- and
    // asio's own thread-safety guarantee is "distinct objects safe, shared objects unsafe unless
    // stated otherwise" (asio docs); basic_stream_socket is not documented as safe for
    // concurrent calls on the SAME instance from two threads, even nominally-independent
    // read/write ones, because of unsynchronized internal state (cached blocking-mode flags,
    // etc). A raw read()/write() pair on the underlying fd has no such shared C++ object state --
    // POSIX guarantees concurrent read() and write() on one socket fd from different threads.
    bool readOne(protocol::DecodedMessage& out) {
        protocol::RejectReason reason;
        for (;;) {
            const auto r = decoder_.next(out, reason);
            if (r == protocol::FrameDecoder::Result::Ok) return true;
            if (r == protocol::FrameDecoder::Result::Invalid) return false;

            std::array<std::byte, 256> buf{};
            const ssize_t got = ::read(socket_.native_handle(), buf.data(), buf.size());
            if (got <= 0) return false;
            if (!decoder_.feed(buf.data(), static_cast<std::size_t>(got))) return false;
        }
    }

    void close() {
        std::error_code ec;
        socket_.close(ec);
    }

 private:
    bool rawWrite(const std::byte* data, std::size_t n) {
        std::size_t sent = 0;
        while (sent < n) {
            const ssize_t w = ::write(socket_.native_handle(), data + sent, n - sent);
            if (w <= 0) return false;
            sent += static_cast<std::size_t>(w);
        }
        return true;
    }

    asio::io_context io_;
    asio::ip::tcp::socket socket_;
    protocol::FrameDecoder decoder_;
};

}  // namespace velox::loadgen
