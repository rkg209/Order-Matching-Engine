#pragma once

// GatewayServer: acceptor, session registry, exec-report router (Spec 007 T4/T5).
//
// One asio::io_context, run by exactly one thread -- this IS the single producer the
// Sequencer requires (decision 4), so submitting a command from a session handler needs no
// lock. A second, dedicated thread drains the outbound MulticastRing (consumer index 0) and
// posts the resulting EXEC_REPORT/REJECT writes back onto the io thread, so the drain never
// waits behind a session's fsync-blocked submit() call.
//
// Not a template: the whole system uses exactly one inbound ring type
// (ipc::SpscRing<ipc::Command>, the same one MatchingThread<> and apps/velox_live.cpp use), and
// templating this on Ring bought nothing but a forward-declaration headache in session.hpp
// (ClientSession needs to name GatewayServer without knowing its template argument).

#include <asio.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gateway/auth.hpp"
#include "gateway/session.hpp"
#include "ipc/command.hpp"
#include "ipc/multicast_ring.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "protocol/encoder.hpp"
#include "protocol/message_types.hpp"
#include "sequencer/sequencer.hpp"

namespace velox::gateway {

// Who to route an EXEC_REPORT/REJECT for a given orderId back to.
struct RouteEntry {
    std::weak_ptr<ClientSession> session;
    std::uint64_t clientSeqNum;
};

class GatewayServer {
 public:
    using InRing = ipc::SpscRing<ipc::Command>;
    using OutRing = ipc::MulticastRing<ipc::OutboundEvent, 2>;

    GatewayServer(asio::io_context& io, sequencer::Sequencer<InRing>& seqr, OutRing& outRing,
                  AuthHandler auth, protocol::InstrumentId instrumentId, Price minPrice,
                  Price maxPrice)
        : io_(io),
          acceptor_(io),
          seqr_(seqr),
          outRing_(outRing),
          auth_(std::move(auth)),
          instrumentId_(instrumentId),
          minPrice_(minPrice),
          maxPrice_(maxPrice) {}

    ~GatewayServer() { stopRouter(); }

    void listen(unsigned short port) {
        asio::ip::tcp::endpoint ep(asio::ip::tcp::v4(), port);
        acceptor_.open(ep.protocol());
        acceptor_.set_option(asio::ip::tcp::acceptor::reuse_address(true));
        acceptor_.bind(ep);
        acceptor_.listen();
        doAccept();
    }

    void startRouter() {
        routerRunning_.store(true, std::memory_order_release);
        routerThread_ = std::thread([this] { runRouter(); });
    }

    void stopRouter() {
        routerRunning_.store(false, std::memory_order_release);
        if (routerThread_.joinable()) {
            routerThread_.join();
        }
    }

    // The bound port -- only meaningful after listen(). Lets tests pass port 0 (OS-assigned)
    // and discover what they actually got.
    unsigned short localPort() const { return acceptor_.local_endpoint().port(); }

    const AuthHandler& auth() const noexcept { return auth_; }
    protocol::InstrumentId instrumentId() const noexcept { return instrumentId_; }
    Price minPrice() const noexcept { return minPrice_; }
    Price maxPrice() const noexcept { return maxPrice_; }

    // Called by a session on the io thread after a NEW_ORDER is durably sequenced, BEFORE the
    // NEW_ACK is sent -- so no EXEC_REPORT/TRADE for this order can ever arrive at the router
    // for an id it hasn't mapped yet (plan T3).
    void registerRoute(OrderId id, std::shared_ptr<ClientSession> session,
                       std::uint64_t clientSeqNum) {
        routes_[id] = RouteEntry{session, clientSeqNum};
    }

    void eraseRoute(OrderId id) { routes_.erase(id); }

    sequencer::Sequencer<InRing>& sequencer() noexcept { return seqr_; }

    // Runs `f` on the io thread. The exec-report router thread uses this for every session
    // write, since asio::ip::tcp::socket is not safe to touch from a second thread
    // concurrently with the io_context's own handlers.
    template<class F>
    void postToIoThread(F&& f) {
        asio::post(io_, std::forward<F>(f));
    }

    // Counters, off the hot path, for observability -- never load-bearing for correctness.
    std::size_t droppedRoutes() const noexcept { return droppedRoutes_.load(); }

 private:
    void doAccept() {
        acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                auto session = std::make_shared<ClientSession>(std::move(socket), *this,
                                                               instrumentId_, minPrice_, maxPrice_);
                sessions_.push_back(session);
                session->start();
            }
            doAccept();
        });
    }

    // Runs on the dedicated router thread. Drains consumer index 0 (exec reports) and, until
    // Spec 008 exists, also drains consumer index 1 as a no-op -- otherwise the MulticastRing's
    // min-cursor gate would stall the matching thread forever (plan T4).
    void runRouter() {
        while (routerRunning_.load(std::memory_order_acquire)) {
            bool any = false;
            const ipc::OutboundEvent* ev;
            while ((ev = outRing_.tryPeek(0)) != nullptr) {
                any = true;
                routeOne(*ev);
                outRing_.consume(0);
            }
            while (outRing_.tryPeek(1) != nullptr) {
                outRing_.consume(1);
            }
            if (any) {
                // Draining consumer 0 may have freed ring space -- give every backpressured
                // session a chance to retry its pending command (FR-28: never drop, only delay).
                postToIoThread([this] { retryAllPending(); });
            } else {
                std::this_thread::yield();
            }
        }
    }

    // io-thread only. Prunes dead weak_ptrs opportunistically while it's here.
    void retryAllPending() {
        auto it = sessions_.begin();
        while (it != sessions_.end()) {
            auto session = it->lock();
            if (!session) {
                it = sessions_.erase(it);
                continue;
            }
            if (session->readSuspended()) {
                session->retryPending();
            }
            ++it;
        }
    }

    void routeOne(const ipc::OutboundEvent& ev) {
        if (ev.kind == ipc::OutboundKind::TradeEvent) {
            const Trade& t = ev.payload.trade;
            routeExecReport(t.aggressorId, protocol::ExecType::Fill, t.quantity, 0, t.price, t.id,
                            ev.globalSeq);
            routeExecReport(t.passiveId, protocol::ExecType::Fill, t.quantity, 0, t.price, t.id,
                            ev.globalSeq);
            return;
        }
        const ipc::StatusChange& sc = ev.payload.statusChange;
        if (sc.status == SubmitStatus::Ok) {
            routeExecReport(sc.orderId, protocol::ExecType::NewAck, 0, 0, 0, 0, ev.globalSeq);
        } else {
            // The wire protocol's RejectReason is deliberately coarse (NFR-26); every engine
            // rejection collapses to EngineReject on the wire, whichever SubmitStatus produced
            // it -- the client learns "the engine rejected this order", not the engine's own
            // internal taxonomy.
            routeReject(sc.orderId, protocol::RejectReason::EngineReject, ev.globalSeq);
        }
    }

    void routeExecReport(OrderId orderId, protocol::ExecType type, Quantity execQty,
                         Quantity leavesQty, Price price, std::int64_t tradeId, Seq globalSeq) {
        auto it = routes_.find(orderId);
        if (it == routes_.end()) {
            ++droppedRoutes_;
            return;
        }
        auto session = it->second.session.lock();
        if (!session) {
            ++droppedRoutes_;
            return;
        }
        protocol::ExecReportMsg m{orderId, type, execQty, leavesQty, price, tradeId, globalSeq};
        std::byte buf[128];
        const std::size_t n = protocol::encodeExecReport(m, buf);
        std::vector<std::byte> owned(buf, buf + n);
        postToIoThread([session, owned = std::move(owned)] {
            session->sendFrame(owned.data(), owned.size());
        });
    }

    void routeReject(OrderId orderId, protocol::RejectReason reason, Seq globalSeq) {
        auto it = routes_.find(orderId);
        if (it == routes_.end()) {
            ++droppedRoutes_;
            return;
        }
        auto session = it->second.session.lock();
        routes_.erase(it);
        if (!session) {
            ++droppedRoutes_;
            return;
        }
        protocol::RejectMsg m{orderId, reason, globalSeq};
        std::byte buf[128];
        const std::size_t n = protocol::encodeReject(m, buf);
        std::vector<std::byte> owned(buf, buf + n);
        postToIoThread([session, owned = std::move(owned)] {
            session->sendFrame(owned.data(), owned.size());
        });
    }

    asio::io_context& io_;
    asio::ip::tcp::acceptor acceptor_;
    sequencer::Sequencer<InRing>& seqr_;
    OutRing& outRing_;
    AuthHandler auth_;
    protocol::InstrumentId instrumentId_;
    Price minPrice_;
    Price maxPrice_;

    std::unordered_map<OrderId, RouteEntry> routes_;
    std::vector<std::weak_ptr<ClientSession>> sessions_;

    std::thread routerThread_;
    std::atomic<bool> routerRunning_{false};
    std::atomic<std::size_t> droppedRoutes_{0};
};

}  // namespace velox::gateway
