#pragma once

// Spec 010 T4: asio TCP client for the latency-stats feed (velox_loadgen --stats-port, one JSON
// line per telemetry::formatStatsLine() call). Passed through UNTOUCHED -- this client's only
// job is TCP framing (splitting the byte stream on '\n'), never parsing the JSON, so the exact
// numbers a browser sees are byte-identical to what the loadgen's reader thread computed.
// Same reconnect-with-backoff and read-only-socket discipline as MdClient (viz/md_client.hpp).

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <chrono>
#include <functional>
#include <string>

#include "viz/read_only_socket.hpp"

namespace velox::viz {

class StatsClient {
 public:
    using OnLine = std::function<void(const std::string&)>;

    StatsClient(asio::io_context& io, std::string host, std::string port)
        : io_(io),
          host_(std::move(host)),
          port_(std::move(port)),
          resolver_(io),
          socket_(io),
          reconnectTimer_(io),
          backoff_(std::chrono::milliseconds(200)) {}

    void start() { connect(); }
    void setOnLine(OnLine cb) { onLine_ = std::move(cb); }
    bool connected() const noexcept { return connected_; }

 private:
    void connect() {
        resolver_.async_resolve(
            host_, port_,
            [this](std::error_code ec, const asio::ip::tcp::resolver::results_type& results) {
                if (ec) {
                    scheduleReconnect();
                    return;
                }
                asio::async_connect(socket_, results,
                                    [this](std::error_code ec2, const asio::ip::tcp::endpoint&) {
                                        if (ec2) {
                                            scheduleReconnect();
                                            return;
                                        }
                                        connected_ = true;
                                        backoff_ = std::chrono::milliseconds(200);
                                        partial_.clear();
                                        doRead();
                                    });
            });
    }

    void doRead() {
        ReadOnlySocket ro(socket_);
        ro.async_read_some(asio::buffer(readBuf_), [this](std::error_code ec, std::size_t n) {
            if (ec) {
                fail();
                return;
            }
            partial_.append(readBuf_.data(), n);
            std::size_t pos;
            while ((pos = partial_.find('\n')) != std::string::npos) {
                const std::string line = partial_.substr(0, pos);
                partial_.erase(0, pos + 1);
                if (onLine_ && !line.empty()) {
                    onLine_(line);
                }
            }
            doRead();
        });
    }

    void fail() {
        connected_ = false;
        scheduleReconnect();
    }

    void scheduleReconnect() {
        ReadOnlySocket(socket_).close();
        socket_ = asio::ip::tcp::socket(io_);
        reconnectTimer_.expires_after(backoff_);
        backoff_ = std::min(backoff_ * 2, std::chrono::milliseconds(5000));
        reconnectTimer_.async_wait([this](std::error_code ec) {
            if (!ec) connect();
        });
    }

    asio::io_context& io_;
    std::string host_;
    std::string port_;
    asio::ip::tcp::resolver resolver_;
    asio::ip::tcp::socket socket_;
    asio::steady_timer reconnectTimer_;
    std::chrono::milliseconds backoff_;

    std::array<char, 1024> readBuf_{};
    std::string partial_;
    OnLine onLine_;
    bool connected_ = false;
};

}  // namespace velox::viz
