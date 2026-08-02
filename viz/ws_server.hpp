#pragma once

// Spec 010 T4: a minimal RFC6455 WebSocket server over asio, sharing one acceptor with a tiny
// static-file server (viz/http_static.hpp) -- the same acceptor serves index.html/app.js/
// style.css to a plain GET and upgrades a WS handshake on the same port, per DR-6 ("one port").
//
// Deliberately minimal: server -> client TEXT frames only (the ladder/histogram JSON this
// visualizer pushes), no permessage-deflate, no fragmentation on the send side. Client -> server
// frames are read, parsed just enough to find the opcode, and discarded -- except CLOSE (close
// the connection) and PING (reply PONG) -- because this feed has nothing for a browser to tell it;
// it never expects a subscribe message. A per-connection bounded outbound queue with drop-on-
// overflow is the same policy as marketdata::FeedSession::enqueue() -- a slow browser tab loses
// its connection, never gets an unbounded buffer.

#include <array>
#include <asio.hpp>
#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "viz/http_static.hpp"

namespace velox::viz {

// A single server->client TEXT frame, RFC6455 5.2: FIN=1/opcode=1, no MASK bit (server frames are
// never masked), with the 125/126/64-bit extended-length encoding. A free function -- not a
// WsSession method -- so tests/viz/ws_handshake_test.cpp can exercise the length boundaries
// directly, without a live socket.
std::vector<std::byte> encodeTextFrame(const std::string& payload);

// The Sec-WebSocket-Accept value RFC6455 4.2.2 defines: base64(sha1(key + the fixed magic GUID)).
// A free function for the same reason as encodeTextFrame() -- tests exercise it against the
// RFC's own worked example directly.
std::string computeAcceptKey(const std::string& secWebSocketKey);

class WsSession : public std::enable_shared_from_this<WsSession> {
 public:
    static constexpr std::size_t kDefaultMaxQueueBytes = 4 * 1024 * 1024;

    explicit WsSession(asio::ip::tcp::socket socket, const StaticFiles* staticFiles,
                       std::size_t maxQueueBytes = kDefaultMaxQueueBytes)
        : socket_(std::move(socket)), staticFiles_(staticFiles), maxQueueBytes_(maxQueueBytes) {}

    void start();

    // Enqueues a text frame. No-op once the session is not an established WS connection (still
    // doing the HTTP dance, or already closed) -- a caller broadcasting to a session list simply
    // skips sessions that were never WS in the first place.
    void sendText(const std::string& payload);

    bool isWs() const noexcept { return isWs_; }
    bool closed() const noexcept { return closed_; }

 private:
    void readHeaders();
    void handleRequestLine(const std::string& requestLine, const std::string& headerBlock);
    void completeHandshake(const std::string& secWebSocketKey);
    void serveHttp(const std::string& urlPath);

    void readFrameHeader();
    void readExtendedLength(std::uint8_t byte1);
    void readMaskKey();
    void readPayload();
    void handleFrame();

    void enqueueRaw(std::vector<std::byte> frame);
    void doWrite();
    void close();

    asio::ip::tcp::socket socket_;
    const StaticFiles* staticFiles_;
    std::size_t maxQueueBytes_;

    asio::streambuf inBuf_;

    // Frame-parsing scratch state (server reads one client frame at a time).
    std::uint8_t opcode_ = 0;
    bool masked_ = false;
    std::uint64_t payloadLen_ = 0;
    std::array<std::uint8_t, 4> maskKey_{};
    std::vector<std::byte> payloadBuf_;

    std::deque<std::vector<std::byte>> pending_;
    std::size_t queuedBytes_ = 0;
    bool writing_ = false;
    bool isWs_ = false;
    bool closed_ = false;
};

class WsServer {
 public:
    explicit WsServer(asio::io_context& io, std::string assetsDir) noexcept
        : acceptor_(io), staticFiles_(std::move(assetsDir)) {}

    void listen(unsigned short port);
    unsigned short localPort() const { return acceptor_.local_endpoint().port(); }

    // Broadcasts one text frame (a ladder or latency-stats JSON line, minus the trailing '\n' the
    // upstream feeds use -- the browser gets one WS message per update, not newline-delimited
    // text) to every currently-connected WS session.
    void broadcastText(const std::string& payload);

    std::size_t sessionCount() const noexcept { return sessions_.size(); }

 private:
    void doAccept();

    asio::ip::tcp::acceptor acceptor_;
    StaticFiles staticFiles_;
    std::vector<std::shared_ptr<WsSession>> sessions_;
};

}  // namespace velox::viz
