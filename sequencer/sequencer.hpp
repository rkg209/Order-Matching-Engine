#pragma once

// The sequencer (Spec 006, FR-16/FR-18/NFR-23): assigns a global monotonic sequence number to
// each inbound command, durably journals it BEFORE acking, then hands it to the matching thread
// via the inbound ring. This -- not the matching thread -- is the authoritative ordering: the
// engine's job is to be a deterministic function of the sequence this class hands out.
//
// Durability precedes acknowledgement, structurally: submit() does not return (and therefore
// cannot push into the ring, since that happens after) until JournalWriter::append() has
// returned, and append() itself does not return until its fsync has returned. There is no path
// from "command entered the ring" back to "command was not yet fsynced" -- the call sequence
// makes it impossible, not just documented.

#include "platform/platform.hpp"
#include "sequencer/journal_writer.hpp"

namespace velox::sequencer {

// Tri-state result for the non-blocking submit path (Spec 007 T3). Sessions must never spin on
// a full ring the way submit() does -- a session that spins blocks the single gateway io
// thread and stalls every other connection, whereas the right response to backpressure here is
// to stop reading the socket and let TCP's own flow control apply (see gateway/session.hpp).
struct TrySubmitResult {
    enum class Outcome { Sequenced, RingFull, DurabilityFailure } outcome;
    Seq seq = 0;
};

template<typename Ring>
class Sequencer {
 public:
    Sequencer(JournalWriter& journal, Ring& ring, Seq startSeq = 0)
        : journal_(journal), ring_(ring), seq_(startSeq) {}

    // Returns the assigned sequence number, or 0 (never a valid sequence -- they start at 1) on
    // a durability failure. On failure the command was NOT acked and was NOT pushed to the ring:
    // a caller must treat this as a hard stop, never retry-and-silently-skip, since skipping
    // would leave a gap the journal reader would later reject as corruption.
    Seq submit(ipc::CommandKind kind, const ipc::Command& cmd) {
        const Seq mySeq = seq_ + 1;
        if (!journal_.append(mySeq, kind, cmd)) {
            return 0;
        }
        seq_ = mySeq;  // only advance after the append (incl. fsync) durably succeeded

        while (!ring_.push(cmd)) {
            platform::cpuPause();
        }
        return mySeq;
    }

    // Non-blocking counterpart of submit() (Spec 007 T3). Checks ring space FIRST, via
    // tryClaim() -- which reserves nothing if it returns nullptr, so a RingFull result has no
    // side effect and can be retried later once the ring drains. Only once a slot is in hand
    // does this journal the command, preserving the same durability-before-visibility ordering
    // submit() uses: the slot is not published until AFTER JournalWriter::append() (incl. its
    // fsync) has returned.
    TrySubmitResult trySubmit(ipc::CommandKind kind, const ipc::Command& cmd) {
        ipc::Command* slot = ring_.tryClaim();
        if (slot == nullptr) {
            return {TrySubmitResult::Outcome::RingFull, 0};
        }
        const Seq mySeq = seq_ + 1;
        if (!journal_.append(mySeq, kind, cmd)) {
            return {TrySubmitResult::Outcome::DurabilityFailure, 0};
        }
        seq_ = mySeq;
        *slot = cmd;
        ring_.publish();
        return {TrySubmitResult::Outcome::Sequenced, mySeq};
    }

    Seq lastSeq() const noexcept { return seq_; }

 private:
    JournalWriter& journal_;
    Ring& ring_;
    Seq seq_;
};

}  // namespace velox::sequencer
