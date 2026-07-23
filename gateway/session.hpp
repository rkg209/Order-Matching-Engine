#pragma once

// ClientSession: one TCP connection, its state machine (Spec 007 T2/T3).
//
// AwaitingLogin -> Authenticated -> Closing. Everything here runs on the single gateway
// io_context thread (decision 4) -- there is exactly one writer per session and no cross-thread
// access to session state except through GatewayServer::postToIoThread (used by the exec-report
// router, T4), so nothing here needs a lock.

#include <array>
#include <asio.hpp>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

#include "common/types.hpp"
#include "ipc/command.hpp"
#include "protocol/decoder.hpp"
#include "protocol/messages.hpp"

namespace velox::gateway {

class GatewayServer;

class ClientSession : public std::enable_shared_from_this<ClientSession> {
 public:
    ClientSession(asio::ip::tcp::socket socket, GatewayServer& server,
                  protocol::InstrumentId instrumentId, Price minPrice, Price maxPrice);

    void start();

    // Enqueues a frame for this session (called from the exec-report router via
    // GatewayServer::postToIoThread, or from within the session's own io-thread handlers). Safe
    // to call re-entrantly: writes are serialized through a private queue, never overlapped.
    void sendFrame(const std::byte* data, std::size_t n);

    ParticipantId participantId() const noexcept { return participantId_; }
    bool authenticated() const noexcept { return state_ == State::Authenticated; }
    bool readSuspended() const noexcept { return readSuspended_; }

    // Called by GatewayServer (io thread) after the router drains the outbound ring, i.e.
    // whenever ring space MAY have freed up. Retries the one command that hit RingFull; a
    // second RingFull leaves the session suspended for the next retry round. This is what makes
    // backpressure lossless (FR-28): the command that didn't fit is never discarded, only
    // delayed, and the socket stops being read meanwhile so TCP applies its own flow control.
    void retryPending();

 private:
    enum class State { AwaitingLogin, Authenticated, Closing };

    static constexpr std::size_t kReadChunk = 128;  // small: keeps FrameDecoder::kCapacity safe

    void doRead();
    void onRead(std::size_t n);
    void processDecoded();
    void doWrite();

    void handleLogin(const protocol::LoginMsg& m);
    void handleNewOrder(const protocol::NewOrderMsg& m);
    void handleCancel(const protocol::CancelMsg& m);
    void handleCancelReplace(const protocol::CancelReplaceMsg& m);
    void handleHeartbeat(const protocol::HeartbeatMsg& m);
    void submitOrPend(const ipc::Command& cmd);
    void sendNewAckIfApplicable(const ipc::Command& cmd, Seq seq);

    // Client sequence check (FR-25). Returns true if `n` is the next expected sequence and the
    // command should be submitted; on a duplicate or a gap it sends the appropriate reject
    // itself and returns false -- the caller must not submit to the engine either way.
    bool checkClientSeq(std::uint64_t n, OrderId orderIdForReject);

    void sendLoginReject(protocol::RejectReason reason);
    void sendReject(OrderId orderId, protocol::RejectReason reason, Seq globalSeq = 0);
    void closeSession();

    void armAuthTimeout();
    void armHeartbeatTimer();

    // Backpressure (FR-28): stop issuing new async_read calls when the ring is full; resumed by
    // GatewayServer once the ring has drained. TCP's own receive window does the rest -- no
    // order is ever dropped waiting for space.
    void stopReading();
    void resumeReading();

    asio::ip::tcp::socket socket_;
    GatewayServer& server_;
    protocol::FrameDecoder decoder_;

    std::array<std::byte, kReadChunk> readBuf_{};
    bool writing_ = false;
    // Pending writes queued while a write is already in flight; small frames only, so a
    // fixed-capacity vector-like buffer would be premature -- a std::deque is fine off the hot
    // path (this is the gateway, the one component allowed to allocate freely).
    std::deque<std::vector<std::byte>> pendingWrites_;

    State state_ = State::AwaitingLogin;
    ParticipantId participantId_ = 0;
    std::uint64_t lastClientSeq_ = 0;
    bool readSuspended_ = false;
    bool closed_ = false;

    // The one command that hit RingFull, kept around for retryPending(). At most one at a time
    // -- reading is suspended the moment this is set, so no second command can arrive to
    // clobber it.
    bool hasPending_ = false;
    ipc::CommandKind pendingKind_ = ipc::CommandKind::New;
    ipc::Command pendingCmd_{};

    asio::steady_timer authTimer_;
    asio::steady_timer heartbeatTimer_;
    int missedHeartbeats_ = 0;

    friend class GatewayServer;
};

}  // namespace velox::gateway
