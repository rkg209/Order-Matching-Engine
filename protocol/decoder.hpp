#pragma once

// FrameDecoder: stateful TCP-stream reassembly + validation (Spec 007 T1).
//
// TCP is a byte stream, not a message protocol -- a frame may arrive split across many reads,
// or several frames may arrive in one read. This class owns a small FIXED reassembly buffer
// (never grows from an attacker-supplied length, which is exactly what a length-prefix DoS
// depends on) and exposes `feed()` / `next()` so a caller can hand it raw socket bytes and pull
// out complete, validated messages one at a time.
//
// Validation order is the security core of this component (see the plan's T1 for the numbered
// list) and is NOT reorderable: length is bound-checked before anything else touches it, and a
// single Invalid result puts the decoder into a terminal state -- there is no attempt to
// resynchronise a length-prefixed stream after a bad length, because that is guesswork, not
// recovery.

#include <cstddef>

#include "common/types.hpp"
#include "protocol/message_types.hpp"
#include "protocol/messages.hpp"

namespace velox::protocol {

// One decoded message, tagged by `type`. Only the member matching `type` is meaningful --
// kept as a flat struct (not a union) because this is off the hot path and simplicity beats
// the handful of extra bytes.
struct DecodedMessage {
    MessageType type;
    LoginMsg login;
    NewOrderMsg newOrder;
    CancelMsg cancel;
    CancelReplaceMsg cancelReplace;
    HeartbeatMsg heartbeat;
    LoginAckMsg loginAck;
    LoginRejectMsg loginReject;
    ExecReportMsg execReport;
    RejectMsg reject;
};

class FrameDecoder {
 public:
    // Comfortably larger than the widest real frame (4 + 1 + 49 = 54 B for EXEC_REPORT), fixed
    // for the lifetime of the decoder -- this is the buffer that never grows from attacker input.
    static constexpr std::size_t kCapacity = 256;

    enum class Result { Ok, Incomplete, Invalid };

    // instrumentId: the single configured instrument (decision 5 -- single-instrument until
    // Spec 011). minPrice/maxPrice: BookConfig bounds, so a LIMIT price out of range is rejected
    // here rather than reaching the engine.
    FrameDecoder(InstrumentId instrumentId, Price minPrice, Price maxPrice) noexcept
        : instrumentId_(instrumentId), minPrice_(minPrice), maxPrice_(maxPrice) {}

    // Appends bytes read from the socket. Returns false if this would overflow the fixed
    // reassembly buffer -- in valid protocol use this cannot happen (a partial frame is bounded
    // by kMaxFrame long before feed() is called again), so a caller seeing false should treat it
    // as hostile input and close the connection.
    bool feed(const std::byte* data, std::size_t n) noexcept;

    // Decodes the next complete frame at the front of the buffer, if any. Call in a loop until
    // Incomplete. On Invalid, the decoder is terminal (see terminal()) and `reason` is set;
    // the caller must close the connection -- no further calls attempt resync.
    Result next(DecodedMessage& out, RejectReason& reason) noexcept;

    bool terminal() const noexcept { return terminal_; }

 private:
    Result fail(RejectReason r, RejectReason& out) noexcept {
        terminal_ = true;
        out = r;
        return Result::Invalid;
    }

    void consume(std::size_t n) noexcept;

    InstrumentId instrumentId_;
    Price minPrice_;
    Price maxPrice_;

    std::byte buf_[kCapacity];
    std::size_t len_ = 0;
    bool terminal_ = false;
};

}  // namespace velox::protocol
