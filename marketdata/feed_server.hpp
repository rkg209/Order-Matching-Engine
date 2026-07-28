#pragma once

// FeedServer: the market-data TCP fan-out (Spec 008 T8). A marketdata::Sink implementation --
// Publisher calls send() once per encoded frame and does not know or care that this is a
// broadcast to N sockets.
//
// Overrun policy (plan's Verification section, DoD bullet 4): each subscriber has its own
// bounded byte queue (default 4 MiB). On overflow the subscriber is DISCONNECTED, never buffered
// further -- unbounded buffering is explicitly rejected as "just a slower way to die". A second,
// healthy subscriber on the same server is unaffected: each FeedSession's queue is independent.
//
// New-subscriber bootstrap: a connection accepted mid-stream cannot just start receiving L2/L3
// deltas -- it has no base state to apply them to. `setSnapshotBurst()` installs a callback that
// replays the CURRENT book (SnapshotStart/.../SnapshotEnd + one L3Order per resting order) to
// exactly one new session, run synchronously inside the accept handler -- before that session is
// added to the broadcast list -- so it is impossible for a live delta to interleave with the
// burst (both run on the same single-threaded io_context, so this ordering is exact, not a race
// merely made unlikely).

#include <array>
#include <asio.hpp>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

#include "marketdata/publisher.hpp"

namespace velox::marketdata {

class FeedSession : public std::enable_shared_from_this<FeedSession> {
 public:
    explicit FeedSession(asio::ip::tcp::socket socket, std::size_t maxQueueBytes) noexcept
        : socket_(std::move(socket)), maxQueueBytes_(maxQueueBytes) {}

    // Starts reading (and discarding) whatever the subscriber sends -- this feed is one-way
    // push, but a socket with no outstanding read never notices the peer closing until the next
    // write fails, which could be a long time on an idle feed.
    void start() { doRead(); }

    // Enqueues a frame. Closes the session if this would exceed the byte budget -- the ONLY
    // response to overflow (plan: never buffer further, never drop silently either -- the
    // subscriber finds out by having its socket closed, and must reconnect to resync).
    void enqueue(const std::byte* data, std::size_t n) {
        if (closed_) {
            return;
        }
        if (queuedBytes_ + n > maxQueueBytes_) {
            close();
            return;
        }
        pending_.emplace_back(data, data + n);
        queuedBytes_ += n;
        if (!writing_) {
            doWrite();
        }
    }

    bool closed() const noexcept { return closed_; }

 private:
    void doRead() {
        auto self = shared_from_this();
        socket_.async_read_some(asio::buffer(readBuf_), [self](std::error_code ec, std::size_t) {
            if (ec) {
                self->close();
                return;
            }
            self->doRead();
        });
    }

    void doWrite() {
        if (pending_.empty()) {
            writing_ = false;
            return;
        }
        writing_ = true;
        auto self = shared_from_this();
        asio::async_write(socket_, asio::buffer(pending_.front()),
                          [self](std::error_code ec, std::size_t n) {
                              if (ec) {
                                  self->close();
                                  return;
                              }
                              self->queuedBytes_ -= n;
                              self->pending_.pop_front();
                              self->doWrite();
                          });
    }

    void close() {
        closed_ = true;
        std::error_code ec;
        socket_.close(ec);
    }

    asio::ip::tcp::socket socket_;
    std::size_t maxQueueBytes_;
    std::deque<std::vector<std::byte>> pending_;
    std::size_t queuedBytes_ = 0;
    std::array<std::byte, 256> readBuf_{};
    bool writing_ = false;
    bool closed_ = false;
};

// Adapts one FeedSession to the Sink interface -- used only for the private per-subscriber
// snapshot burst (see class doc); the broadcast path talks to FeedSession directly.
class SessionSink : public Sink {
 public:
    explicit SessionSink(std::shared_ptr<FeedSession> session) : session_(std::move(session)) {}
    void send(const std::byte* data, std::size_t n) override { session_->enqueue(data, n); }

 private:
    std::shared_ptr<FeedSession> session_;
};

class FeedServer : public Sink {
 public:
    static constexpr std::size_t kDefaultMaxQueueBytes = 4 * 1024 * 1024;

    explicit FeedServer(asio::io_context& io,
                        std::size_t maxQueueBytes = kDefaultMaxQueueBytes) noexcept
        : io_(io), acceptor_(io), maxQueueBytes_(maxQueueBytes) {}

    void listen(unsigned short port) {
        asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();
        doAccept();
    }

    unsigned short localPort() const { return acceptor_.local_endpoint().port(); }

    // `fn(Sink&)` replays the current book to exactly the one session passed in. Must be set
    // before listen() accepts its first connection to matter; a server with none installed
    // simply starts new subscribers with no base state (fine for tests that only care about the
    // live delta stream).
    void setSnapshotBurst(std::function<void(Sink&)> fn) { snapshotBurst_ = std::move(fn); }

    std::size_t sessionCount() const noexcept { return sessions_.size(); }

    // marketdata::Sink: called by Publisher once per encoded frame, from whichever thread runs
    // Publisher::pump() -- asio::ip::tcp::socket is not safe to touch from a second thread
    // concurrently with the io_context's own handlers, so this always posts rather than writing
    // directly.
    void send(const std::byte* data, std::size_t n) override {
        std::vector<std::byte> owned(data, data + n);
        // dispatch, not post: if the caller is already running ON the io thread (e.g. tests, or
        // a future single-threaded wiring), this runs immediately and in-order with no queueing
        // hop -- post() would still be correct, just an unnecessary round trip in that case.
        asio::dispatch(io_,
                       [this, owned = std::move(owned)] { broadcast(owned.data(), owned.size()); });
    }

 private:
    void doAccept() {
        acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<FeedSession>(std::move(socket), maxQueueBytes_);
                session->start();
                if (snapshotBurst_) {
                    // Synchronous, on the io thread, BEFORE this session joins sessions_ -- see
                    // the class doc for why this makes the burst-then-live ordering exact.
                    SessionSink sink(session);
                    snapshotBurst_(sink);
                }
                sessions_.push_back(session);
            }
            doAccept();
        });
    }

    void broadcast(const std::byte* data, std::size_t n) {
        auto it = sessions_.begin();
        while (it != sessions_.end()) {
            if ((*it)->closed()) {
                it = sessions_.erase(it);
                continue;
            }
            (*it)->enqueue(data, n);
            ++it;
        }
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    std::size_t maxQueueBytes_;
    std::function<void(Sink&)> snapshotBurst_;
    std::vector<std::shared_ptr<FeedSession>> sessions_;
};

}  // namespace velox::marketdata
