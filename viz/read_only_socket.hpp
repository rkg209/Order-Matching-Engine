#pragma once

// Spec 010 T4 / CON-7: the mechanical half of "strictly read-only". Every upstream connection
// (market-data, latency-stats) is read through THIS type, not the raw asio::ip::tcp::socket --
// it exposes only async_read_some and close(), so there is no write-capable API in scope at any
// call site that talks to the engine's side of the wire. This does not replace
// tests/viz/readonly_test.cpp's socket-level proof (a type system can be worked around; a byte
// counter on a real proxy cannot), but it does mean an accidental `socket.async_write(...)` on an
// upstream connection is a compile error, not a code-review miss.

#include <asio.hpp>

namespace velox::viz {

class ReadOnlySocket {
 public:
    explicit ReadOnlySocket(asio::ip::tcp::socket& socket) noexcept : socket_(socket) {}

    template<class MutableBufferSequence, class ReadHandler>
    void async_read_some(const MutableBufferSequence& buffers, ReadHandler&& handler) {
        socket_.async_read_some(buffers, std::forward<ReadHandler>(handler));
    }

    void close() noexcept {
        std::error_code ec;
        socket_.close(ec);
    }

 private:
    asio::ip::tcp::socket& socket_;
};

}  // namespace velox::viz
