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

    Seq lastSeq() const noexcept { return seq_; }

 private:
    JournalWriter& journal_;
    Ring& ring_;
    Seq seq_;
};

}  // namespace velox::sequencer
