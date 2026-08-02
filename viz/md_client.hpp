#pragma once

// Spec 010 T4: asio TCP client -> marketdata::FeedDecoder -> viz::Ladder. Reconnects with
// exponential backoff on any close -- a fresh connection re-triggers the server's snapshot burst
// (marketdata::FeedServer's doc), so state re-syncs for free rather than needing bespoke gap-
// recovery logic here. Being dropped by the upstream FeedServer for being slow is the DESIGNED
// backpressure response (marketdata::FeedSession::enqueue()), not a bug to work around.
//
// Read-only by construction: every read on the upstream socket goes through ReadOnlySocket
// (viz/read_only_socket.hpp), which exposes no write method at all.

#include <algorithm>
#include <array>
#include <asio.hpp>
#include <chrono>
#include <cstddef>
#include <string>

#include "marketdata/feed_decoder.hpp"
#include "protocol/message_types.hpp"
#include "viz/ladder.hpp"
#include "viz/read_only_socket.hpp"

namespace velox::viz {

class MdClient {
 public:
    MdClient(asio::io_context& io, std::string host, std::string port)
        : io_(io),
          host_(std::move(host)),
          port_(std::move(port)),
          resolver_(io),
          socket_(io),
          reconnectTimer_(io),
          backoff_(std::chrono::milliseconds(200)) {}

    void start() { connect(); }

    const Ladder& ladder() const noexcept { return ladder_; }
    std::uint64_t lastSeq() const noexcept { return lastSeq_; }
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
                                        decoder_ = marketdata::FeedDecoder{};
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
            if (!decoder_.feed(reinterpret_cast<const std::byte*>(readBuf_.data()), n)) {
                fail();
                return;
            }
            marketdata::DecodedFeedMessage msg;
            for (;;) {
                const auto r = decoder_.next(msg);
                if (r == marketdata::FeedDecoder::Result::Incomplete) break;
                if (r == marketdata::FeedDecoder::Result::Invalid) {
                    fail();
                    return;
                }
                dispatch(msg);
            }
            doRead();
        });
    }

    void dispatch(const marketdata::DecodedFeedMessage& msg) {
        switch (msg.type) {
            case protocol::MessageType::SnapshotStart:
                ladder_.onSnapshotStart();
                lastSeq_ = msg.snapshotStart.feedSeq;
                return;
            case protocol::MessageType::SnapshotEnd:
                ladder_.onSnapshotEnd();
                lastSeq_ = msg.snapshotEnd.feedSeq;
                return;
            case protocol::MessageType::L3Order:
                ladder_.onL3Order(msg.l3Order);
                lastSeq_ = msg.l3Order.feedSeq;
                return;
            case protocol::MessageType::L2Delta:
                ladder_.onL2Delta(msg.l2Delta);
                lastSeq_ = msg.l2Delta.feedSeq;
                return;
            case protocol::MessageType::TradeTick:
                ladder_.onTrade(msg.tradeTick);
                lastSeq_ = msg.tradeTick.feedSeq;
                return;
            case protocol::MessageType::L3Fill:
                lastSeq_ = msg.l3Fill.feedSeq;
                return;
            default:
                return;  // not a market-data message type; the decoder never produces one anyway
        }
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

    std::array<char, 4096> readBuf_{};
    marketdata::FeedDecoder decoder_;
    Ladder ladder_;
    std::uint64_t lastSeq_ = 0;
    bool connected_ = false;
};

}  // namespace velox::viz
