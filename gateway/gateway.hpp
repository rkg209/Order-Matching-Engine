#pragma once

// GatewayServer: acceptor, session registry, exec-report router (Spec 007 T4/T5; sharded per
// Spec 011 T3).
//
// One asio::io_context, run by exactly one thread -- this IS the single producer every shard's
// Sequencer requires (decision 4), so submitting a command from a session handler needs no lock:
// one thread pushing into N different rings satisfies SPSC N times over, without changing the
// ring or adding a lock to the submit path. One dedicated router thread PER SHARD drains that
// shard's outbound MulticastRing (consumer index 0) and posts the resulting EXEC_REPORT/REJECT
// writes back onto the io thread. A single round-robin router across N rings would make a slow
// session on shard 0 delay shard 1's exec reports -- exactly the isolation Spec 011 exists to
// prove -- so each shard gets its own router thread and its own `routes_` map instead.
//
// `ShardCtx::routesMutex` fixes a pre-existing data race in the single-shard Spec 007 code:
// `routes_` was written by the io thread (registerRoute/eraseRoute) and read+erased by the router
// thread (routeExecReport/routeReject) with no synchronization at all. Sharding would otherwise
// multiply that race by N. The gateway is explicitly permitted to lock (CLAUDE.md: off-hot-path
// components may allocate and lock freely) and none of this is on the matching hot path -- the
// lock is held only around the map find/erase, never around the encode or the io-thread post.
//
// Not a template: the whole system uses exactly one inbound ring type
// (ipc::SpscRing<ipc::Command>, the same one MatchingThread<> and apps/velox_live.cpp use), and
// templating this on Ring bought nothing but a forward-declaration headache in session.hpp
// (ClientSession needs to name GatewayServer without knowing its template argument).

#include <asio.hpp>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gateway/auth.hpp"
#include "gateway/session.hpp"
#include "ipc/command.hpp"
#include "ipc/multicast_ring.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"
#include "protocol/decoder.hpp"
#include "protocol/encoder.hpp"
#include "protocol/message_types.hpp"
#include "runtime/shard.hpp"
#include "sequencer/sequencer.hpp"

namespace velox::gateway {

// Who to route an EXEC_REPORT/REJECT for a given orderId back to.
struct RouteEntry {
    std::weak_ptr<ClientSession> session;
    std::uint64_t clientSeqNum;
};

class GatewayServer {
 public:
    using InRing = runtime::Shard::InRing;
    // Must match runtime::MatchingThread<>::OutRing exactly (same GatingMask) -- this is the
    // ring each shard's matching thread actually publishes into, and index 1 (market data, Spec
    // 008) is non-gating there.
    using OutRing = runtime::Shard::OutRing;

    GatewayServer(asio::io_context& io, runtime::ShardSet& shards, AuthHandler auth, Price minPrice,
                  Price maxPrice)
        : io_(io),
          acceptor_(io),
          shards_(shards),
          auth_(std::move(auth)),
          instruments_(shards.instrumentSet()),
          minPrice_(minPrice),
          maxPrice_(maxPrice) {
        shardCtxs_.reserve(shards_.size());
        for (std::size_t i = 0; i < shards_.size(); ++i) {
            shardCtxs_.push_back(std::make_unique<ShardCtx>(&shards_[i]));
        }
    }

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
        for (std::size_t i = 0; i < shardCtxs_.size(); ++i) {
            shardCtxs_[i]->routerThread = std::thread([this, i] { runRouter(i); });
        }
    }

    void stopRouter() {
        routerRunning_.store(false, std::memory_order_release);
        for (auto& ctx : shardCtxs_) {
            if (ctx->routerThread.joinable()) {
                ctx->routerThread.join();
            }
        }
    }

    // The bound port -- only meaningful after listen(). Lets tests pass port 0 (OS-assigned)
    // and discover what they actually got.
    unsigned short localPort() const { return acceptor_.local_endpoint().port(); }

    const AuthHandler& auth() const noexcept { return auth_; }
    const protocol::InstrumentSet& instruments() const noexcept { return instruments_; }
    Price minPrice() const noexcept { return minPrice_; }
    Price maxPrice() const noexcept { return maxPrice_; }

    // -1 if `id` names no configured shard. Session handlers call this once per NEW_ORDER/
    // CANCEL/CANCEL_REPLACE to resolve which shard's ring/sequencer/routes to use.
    int shardIndexFor(protocol::InstrumentId id) const noexcept { return shards_.indexOf(id); }

    sequencer::Sequencer<InRing>& shardSequencer(int shardIdx) noexcept {
        return shardCtxs_[static_cast<std::size_t>(shardIdx)]->shard->sequencer();
    }

    // LoginAckMsg::serverSeq is informational only (protocol/messages.hpp) -- with N independent
    // per-shard sequence spaces (plan's "per-shard journals, not one global sequencer") there is
    // no single number that means "the" server sequence anymore. Shard 0's is reported, same as
    // every pre-sharding single-instrument deployment would report its only shard's.
    Seq loginAckSeq() const noexcept {
        return shardCtxs_.empty() ? 0 : shardCtxs_.front()->shard->sequencer().lastSeq();
    }

    // Called by a session on the io thread after a NEW_ORDER is durably sequenced, BEFORE the
    // NEW_ACK is sent -- so no EXEC_REPORT/TRADE for this order can ever arrive at the router
    // for an id it hasn't mapped yet (plan T3).
    void registerRoute(int shardIdx, OrderId id, std::shared_ptr<ClientSession> session,
                       std::uint64_t clientSeqNum) {
        ShardCtx& ctx = *shardCtxs_[static_cast<std::size_t>(shardIdx)];
        std::lock_guard<std::mutex> lock(ctx.routesMutex);
        ctx.routes[id] = RouteEntry{session, clientSeqNum};
    }

    void eraseRoute(int shardIdx, OrderId id) {
        ShardCtx& ctx = *shardCtxs_[static_cast<std::size_t>(shardIdx)];
        std::lock_guard<std::mutex> lock(ctx.routesMutex);
        ctx.routes.erase(id);
    }

    // Runs `f` on the io thread. The exec-report router threads use this for every session
    // write, since asio::ip::tcp::socket is not safe to touch from a second thread
    // concurrently with the io_context's own handlers.
    template<class F>
    void postToIoThread(F&& f) {
        asio::post(io_, std::forward<F>(f));
    }

    // Counters, off the hot path, for observability -- never load-bearing for correctness.
    std::size_t droppedRoutes() const noexcept {
        std::size_t total = 0;
        for (const auto& ctx : shardCtxs_) {
            total += ctx->droppedRoutes.load(std::memory_order_relaxed);
        }
        return total;
    }
    std::size_t droppedRoutesFor(std::size_t shardIdx) const noexcept {
        return shardCtxs_[shardIdx]->droppedRoutes.load(std::memory_order_relaxed);
    }

 private:
    // Bundles everything the router thread for one shard needs, plus that shard's routes_ table
    // and the mutex protecting it (T3b: fixes the pre-existing race). Not copyable/movable
    // (owns a std::mutex and a std::thread) -- held via unique_ptr in shardCtxs_ so the vector
    // itself can still grow/reserve during construction.
    struct ShardCtx {
        explicit ShardCtx(runtime::Shard* s) : shard(s) {}
        runtime::Shard* shard;
        std::mutex routesMutex;
        std::unordered_map<OrderId, RouteEntry> routes;
        std::thread routerThread;
        std::atomic<std::size_t> droppedRoutes{0};
    };

    void doAccept() {
        acceptor_.async_accept([this](std::error_code ec, asio::ip::tcp::socket socket) {
            if (!ec) {
                // Every message on this protocol (NEW_ORDER, EXEC_REPORT, ...) is small and its
                // own frame -- Nagle's algorithm coalescing them with delayed ACKs turns a
                // sub-millisecond exec report into a multi-millisecond one for no reason a
                // latency-focused gateway should ever accept.
                std::error_code ndEc;
                socket.set_option(asio::ip::tcp::no_delay(true), ndEc);
                auto session = std::make_shared<ClientSession>(std::move(socket), *this,
                                                               instruments_, minPrice_, maxPrice_);
                sessions_.push_back(session);
                session->start();
            }
            doAccept();
        });
    }

    // Runs on shard `shardIdx`'s dedicated router thread. Drains consumer index 0 (exec reports)
    // only -- index 1 (market data, Spec 008) is non-gating (GatingMask), so this thread does not
    // need to touch it at all, and must not: marketdata::Publisher is index 1's one and only
    // reader for this shard.
    void runRouter(std::size_t shardIdx) {
        ShardCtx& ctx = *shardCtxs_[shardIdx];
        OutRing& outRing = ctx.shard->outRing();
        while (routerRunning_.load(std::memory_order_acquire)) {
            bool any = false;
            const ipc::OutboundEvent* ev;
            while ((ev = outRing.tryPeek(0)) != nullptr) {
                any = true;
                routeOne(shardIdx, ctx, *ev);
                outRing.consume(0);
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

    // io-thread only. Prunes dead weak_ptrs opportunistically while it's here. Walks ALL
    // sessions regardless of which shard's router triggered the retry: a session may be
    // backpressured on any shard, and the router thread that just drained does not know which
    // sessions are waiting on it without also tracking that per-shard, which buys nothing here.
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

    void routeOne(std::size_t shardIdx, ShardCtx& ctx, const ipc::OutboundEvent& ev) {
        switch (ev.kind) {
            case ipc::OutboundKind::TradeEvent: {
                const Trade& t = ev.payload.trade;
                routeExecReport(shardIdx, ctx, t.aggressorId, protocol::ExecType::Fill, t.quantity,
                                0, t.price, t.id, ev.globalSeq);
                routeExecReport(shardIdx, ctx, t.passiveId, protocol::ExecType::Fill, t.quantity, 0,
                                t.price, t.id, ev.globalSeq);
                return;
            }
            case ipc::OutboundKind::StatusEvent: {
                const ipc::StatusChange& sc = ev.payload.statusChange;
                if (sc.status == SubmitStatus::Ok) {
                    routeExecReport(shardIdx, ctx, sc.orderId, protocol::ExecType::NewAck, 0, 0, 0,
                                    0, ev.globalSeq);
                } else {
                    // The wire protocol's RejectReason is deliberately coarse (NFR-26); every
                    // engine rejection collapses to EngineReject on the wire, whichever
                    // SubmitStatus produced it -- the client learns "the engine rejected this
                    // order", not the engine's own internal taxonomy.
                    routeReject(shardIdx, ctx, sc.orderId, protocol::RejectReason::EngineReject,
                                ev.globalSeq);
                }
                return;
            }
            case ipc::OutboundKind::OrderUpdate:
                // Spec 008: L3 events exist for the market-data publisher (consumer index 1),
                // not the exec-report router -- every outcome an OrderUpdate could report is
                // already covered on this path by a TradeEvent and/or a StatusEvent.
                return;
        }
    }

    void routeExecReport(std::size_t shardIdx, ShardCtx& ctx, OrderId orderId,
                         protocol::ExecType type, Quantity execQty, Quantity leavesQty, Price price,
                         std::int64_t tradeId, Seq globalSeq) {
        std::shared_ptr<ClientSession> session;
        {
            std::lock_guard<std::mutex> lock(ctx.routesMutex);
            auto it = ctx.routes.find(orderId);
            if (it == ctx.routes.end()) {
                ++ctx.droppedRoutes;
                return;
            }
            session = it->second.session.lock();
        }
        if (!session) {
            ++ctx.droppedRoutes;
            (void)shardIdx;
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

    void routeReject(std::size_t shardIdx, ShardCtx& ctx, OrderId orderId,
                     protocol::RejectReason reason, Seq globalSeq) {
        std::shared_ptr<ClientSession> session;
        {
            std::lock_guard<std::mutex> lock(ctx.routesMutex);
            auto it = ctx.routes.find(orderId);
            if (it == ctx.routes.end()) {
                ++ctx.droppedRoutes;
                return;
            }
            session = it->second.session.lock();
            ctx.routes.erase(it);
        }
        if (!session) {
            ++ctx.droppedRoutes;
            (void)shardIdx;
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
    runtime::ShardSet& shards_;
    AuthHandler auth_;
    protocol::InstrumentSet instruments_;
    Price minPrice_;
    Price maxPrice_;

    std::vector<std::unique_ptr<ShardCtx>> shardCtxs_;
    std::vector<std::weak_ptr<ClientSession>> sessions_;

    std::atomic<bool> routerRunning_{false};
};

}  // namespace velox::gateway
