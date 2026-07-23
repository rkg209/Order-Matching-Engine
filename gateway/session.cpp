#include "gateway/session.hpp"

#include <chrono>

#include "gateway/gateway.hpp"
#include "ipc/command.hpp"

namespace velox::gateway {

namespace {
constexpr auto kAuthTimeout = std::chrono::seconds(5);
constexpr auto kHeartbeatInterval = std::chrono::seconds(30);
constexpr int kMaxMissedHeartbeats = 3;
}  // namespace

ClientSession::ClientSession(asio::ip::tcp::socket socket, GatewayServer& server,
                             protocol::InstrumentId instrumentId, Price minPrice, Price maxPrice)
    : socket_(std::move(socket)),
      server_(server),
      decoder_(instrumentId, minPrice, maxPrice),
      authTimer_(socket_.get_executor()),
      heartbeatTimer_(socket_.get_executor()) {}

void ClientSession::start() {
    armAuthTimeout();
    armHeartbeatTimer();
    doRead();
}

void ClientSession::armAuthTimeout() {
    authTimer_.expires_after(kAuthTimeout);
    auto self = shared_from_this();
    authTimer_.async_wait([this, self](std::error_code ec) {
        if (ec || state_ != State::AwaitingLogin) {
            return;  // cancelled (LOGIN arrived) or already closing
        }
        closeSession();  // NFR-28: idle unauthenticated connection
    });
}

void ClientSession::armHeartbeatTimer() {
    heartbeatTimer_.expires_after(kHeartbeatInterval);
    auto self = shared_from_this();
    heartbeatTimer_.async_wait([this, self](std::error_code ec) {
        if (ec || state_ == State::Closing) {
            return;
        }
        ++missedHeartbeats_;
        if (missedHeartbeats_ >= kMaxMissedHeartbeats) {
            closeSession();
            return;
        }
        protocol::HeartbeatMsg hb{0};
        std::byte buf[32];
        const std::size_t n = protocol::encodeHeartbeat(hb, buf);
        sendFrame(buf, n);
        armHeartbeatTimer();
    });
}

void ClientSession::doRead() {
    if (closed_ || readSuspended_) {
        return;
    }
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(readBuf_.data(), readBuf_.size()),
                            [this, self](std::error_code ec, std::size_t n) {
                                if (ec) {
                                    closeSession();
                                    return;
                                }
                                onRead(n);
                            });
}

void ClientSession::onRead(std::size_t n) {
    missedHeartbeats_ = 0;
    if (!decoder_.feed(readBuf_.data(), n)) {
        closeSession();  // reassembly buffer would have overflowed -- treat as hostile
        return;
    }
    processDecoded();
    if (!closed_) {
        doRead();
    }
}

void ClientSession::processDecoded() {
    protocol::DecodedMessage msg;
    protocol::RejectReason reason;
    for (;;) {
        const auto result = decoder_.next(msg, reason);
        if (result == protocol::FrameDecoder::Result::Incomplete) {
            return;
        }
        if (result == protocol::FrameDecoder::Result::Invalid) {
            // FR-24 / NFR-26: a malformed frame is a terminal condition for this connection --
            // no attempt to resynchronise a length-prefixed stream after a bad length.
            closeSession();
            return;
        }

        if (state_ == State::AwaitingLogin) {
            if (msg.type == protocol::MessageType::Login) {
                handleLogin(msg.login);
            } else if (msg.type == protocol::MessageType::Heartbeat) {
                // Heartbeats are allowed before auth so a well-behaved client's idle timer
                // doesn't fire while it is still connecting; anything else is rejected (FR-24).
                handleHeartbeat(msg.heartbeat);
            } else {
                sendLoginReject(protocol::RejectReason::NotAuthenticated);
                closeSession();
                return;
            }
            if (closed_) return;
            continue;
        }

        switch (msg.type) {
            case protocol::MessageType::NewOrder:
                handleNewOrder(msg.newOrder);
                break;
            case protocol::MessageType::Cancel:
                handleCancel(msg.cancel);
                break;
            case protocol::MessageType::CancelReplace:
                handleCancelReplace(msg.cancelReplace);
                break;
            case protocol::MessageType::Heartbeat:
                handleHeartbeat(msg.heartbeat);
                break;
            case protocol::MessageType::Login:
                // Logging in twice: reject, do not re-authenticate silently.
                sendLoginReject(protocol::RejectReason::AlreadyAuthenticated);
                closeSession();
                return;
            default:
                closeSession();
                return;
        }
        if (closed_ || readSuspended_) {
            return;
        }
    }
}

void ClientSession::handleLogin(const protocol::LoginMsg& m) {
    if (!server_.auth().authenticate(m.participantId, m.token)) {
        sendLoginReject(protocol::RejectReason::AuthFailed);
        closeSession();
        return;
    }
    participantId_ = m.participantId;
    lastClientSeq_ = m.clientSeqNum;
    state_ = State::Authenticated;
    authTimer_.cancel();

    protocol::LoginAckMsg ack{server_.sequencer().lastSeq(), 0};
    std::byte buf[64];
    const std::size_t n = protocol::encodeLoginAck(ack, buf);
    sendFrame(buf, n);
}

void ClientSession::sendLoginReject(protocol::RejectReason reason) {
    protocol::LoginRejectMsg m{reason, 0};
    std::byte buf[16];
    const std::size_t n = protocol::encodeLoginReject(m, buf);
    sendFrame(buf, n);
}

bool ClientSession::checkClientSeq(std::uint64_t n, OrderId orderIdForReject) {
    if (n <= lastClientSeq_) {
        // Duplicate/retry (FR-25): reject, do NOT submit to the engine -- this is the
        // idempotency guarantee a retried order must not double-execute.
        sendReject(orderIdForReject, protocol::RejectReason::DuplicateSeq);
        return false;
    }
    if (n > lastClientSeq_ + 1) {
        // Gap: this session's stream is no longer trustworthy.
        sendReject(orderIdForReject, protocol::RejectReason::SequenceGap);
        closeSession();
        return false;
    }
    lastClientSeq_ = n;
    return true;
}

void ClientSession::handleNewOrder(const protocol::NewOrderMsg& m) {
    if (!checkClientSeq(m.clientSeqNum, m.orderId)) {
        return;
    }

    ipc::Command cmd{};
    cmd.id = m.orderId;
    cmd.newId = 0;
    cmd.price = m.price;
    cmd.quantity = m.quantity;
    cmd.participant = participantId_;
    cmd.kind = ipc::CommandKind::New;
    cmd.side = m.side;
    cmd.type = m.orderType == protocol::WireOrderType::Market
                   ? OrderType::Market
                   : (m.timeInForce == protocol::WireTimeInForce::Ioc   ? OrderType::Ioc
                      : m.timeInForce == protocol::WireTimeInForce::Fok ? OrderType::Fok
                                                                        : OrderType::Limit);

    submitOrPend(cmd);
}

void ClientSession::handleCancel(const protocol::CancelMsg& m) {
    if (!checkClientSeq(m.clientSeqNum, m.orderId)) {
        return;
    }
    ipc::Command cmd{};
    cmd.id = m.orderId;
    cmd.participant = participantId_;
    cmd.kind = ipc::CommandKind::Cancel;
    submitOrPend(cmd);
}

void ClientSession::handleCancelReplace(const protocol::CancelReplaceMsg& m) {
    if (!checkClientSeq(m.clientSeqNum, m.orderId)) {
        return;
    }
    ipc::Command cmd{};
    cmd.id = m.orderId;     // OLD id
    cmd.newId = m.orderId;  // replace keeps the same id on this wire (no new-id field)
    cmd.price = m.newPrice;
    cmd.quantity = m.newQuantity;
    cmd.participant = participantId_;
    cmd.kind = ipc::CommandKind::Replace;
    cmd.type = OrderType::Limit;
    submitOrPend(cmd);
}

// Shared by NEW_ORDER/CANCEL/CANCEL_REPLACE: register the route BEFORE the ring is touched
// (plan T3 -- no EXEC_REPORT for this order can arrive at the router before it knows where to
// send it), then submit. RingFull suspends reading and keeps the command for retryPending()
// instead of rejecting it -- backpressure must never drop an order (FR-28).
void ClientSession::submitOrPend(const ipc::Command& cmd) {
    server_.registerRoute(cmd.kind == ipc::CommandKind::Replace ? cmd.newId : cmd.id,
                          shared_from_this(), lastClientSeq_);
    const sequencer::TrySubmitResult r = server_.sequencer().trySubmit(cmd.kind, cmd);
    switch (r.outcome) {
        case sequencer::TrySubmitResult::Outcome::Sequenced:
            sendNewAckIfApplicable(cmd, r.seq);
            break;
        case sequencer::TrySubmitResult::Outcome::RingFull:
            hasPending_ = true;
            pendingKind_ = cmd.kind;
            pendingCmd_ = cmd;
            stopReading();
            break;
        case sequencer::TrySubmitResult::Outcome::DurabilityFailure: {
            const OrderId routedId = cmd.kind == ipc::CommandKind::Replace ? cmd.newId : cmd.id;
            server_.eraseRoute(routedId);
            sendReject(routedId, protocol::RejectReason::EngineReject);
            closeSession();
            break;
        }
    }
}

void ClientSession::retryPending() {
    if (!hasPending_ || closed_) {
        return;
    }
    const sequencer::TrySubmitResult r = server_.sequencer().trySubmit(pendingKind_, pendingCmd_);
    if (r.outcome == sequencer::TrySubmitResult::Outcome::RingFull) {
        return;  // still full; wait for the next retry round
    }
    hasPending_ = false;
    if (r.outcome == sequencer::TrySubmitResult::Outcome::DurabilityFailure) {
        const OrderId routedId =
            pendingCmd_.kind == ipc::CommandKind::Replace ? pendingCmd_.newId : pendingCmd_.id;
        server_.eraseRoute(routedId);
        sendReject(routedId, protocol::RejectReason::EngineReject);
        closeSession();
        return;
    }
    sendNewAckIfApplicable(pendingCmd_, r.seq);
    resumeReading();
}

// NEW_ACK is emitted here, synchronously, the moment trySubmit() durably sequences a NEW_ORDER
// (plan decision 3) -- not via the exec-report router, because MatchingThread::dispatch() does
// NOT publish a StatusEvent for a successful New (that would cost the hot path a ring write per
// order for no reason; see runtime/matching_thread.hpp). CANCEL/CANCEL_REPLACE are different:
// dispatch() always publishes a StatusEvent for them, Ok or not, so their acks/rejects arrive
// through the router once the engine actually processes them.
void ClientSession::sendNewAckIfApplicable(const ipc::Command& cmd, Seq seq) {
    if (cmd.kind != ipc::CommandKind::New) {
        return;
    }
    protocol::ExecReportMsg m{cmd.id, protocol::ExecType::NewAck, 0, cmd.quantity, 0, 0, seq};
    std::byte buf[128];
    const std::size_t n = protocol::encodeExecReport(m, buf);
    sendFrame(buf, n);
}

void ClientSession::handleHeartbeat(const protocol::HeartbeatMsg&) {
    missedHeartbeats_ = 0;
}

void ClientSession::sendReject(OrderId orderId, protocol::RejectReason reason, Seq globalSeq) {
    protocol::RejectMsg m{orderId, reason, globalSeq};
    std::byte buf[32];
    const std::size_t n = protocol::encodeReject(m, buf);
    sendFrame(buf, n);
}

void ClientSession::sendFrame(const std::byte* data, std::size_t n) {
    if (closed_) {
        return;
    }
    pendingWrites_.emplace_back(data, data + n);
    if (!writing_) {
        doWrite();
    }
}

void ClientSession::doWrite() {
    if (pendingWrites_.empty()) {
        writing_ = false;
        return;
    }
    writing_ = true;
    auto self = shared_from_this();
    const auto& front = pendingWrites_.front();
    asio::async_write(socket_, asio::buffer(front.data(), front.size()),
                      [this, self](std::error_code ec, std::size_t) {
                          if (ec) {
                              closeSession();
                              return;
                          }
                          pendingWrites_.pop_front();
                          doWrite();
                      });
}

void ClientSession::stopReading() {
    readSuspended_ = true;
}

void ClientSession::resumeReading() {
    if (!readSuspended_ || closed_) {
        return;
    }
    readSuspended_ = false;
    doRead();
}

void ClientSession::closeSession() {
    if (closed_) {
        return;
    }
    closed_ = true;
    state_ = State::Closing;
    std::error_code ec;
    socket_.close(ec);
    authTimer_.cancel();
    heartbeatTimer_.cancel();
}

}  // namespace velox::gateway
