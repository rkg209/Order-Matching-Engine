#pragma once

// Live journal tailing (Spec 012, T1). Wraps JournalReader for a reader racing a JournalWriter
// that is still appending, rather than replaying a journal frozen at recovery time.
//
// The one semantic that differs from recovery, and it is the crux of this file:
// `ReadStatus::TruncatedTail` means "this is the final, torn write" to RecoveryManager, which
// only ever reads a journal nothing will append to again -- but to a tailer racing a live
// writer, the exact same byte pattern means "the record is still being written; come back."
// This tailer maps TruncatedTail -> NotYet and retries. `Corrupt` (a sequence gap mid-journal)
// stays fatal in both readers: that can never legitimately happen against a live writer either.
//
// The other thing a live tailer needs that recovery never did: JournalReader snapshots a
// segment's file size when it opens it and never re-checks it (by design, for a one-shot
// reader). If tailing catches up to that stale size, JournalReader treats it as clean EOF and
// closes the segment for good -- even though the writer may still be appending to that exact
// file. This tailer always resumes-by-reopening (via seekTo) rather than trusting a held-open
// fd across a "nothing new yet" result, so it can never get stuck on a segment that looked done
// but wasn't.

#include <cstdint>
#include <filesystem>

#include "ipc/command.hpp"
#include "sequencer/journal_reader.hpp"

namespace velox::sequencer {

enum class TailStatus { Ok, NotYet, Corrupt };

// lastGoodSeq == 0 means "start from the beginning of the journal directory" -- Seq values are
// documented to start at 1 (sequencer/sequencer.hpp), so 0 is never a real resume point. Note
// createdCounter is NOT a safe sentinel on its own: JournalWriter's very first segment is
// legitimately createdCounter == 0.
struct Checkpoint {
    std::uint64_t createdCounter = 0;
    std::size_t offset = 0;
    Seq lastGoodSeq = 0;
};

struct TailResult {
    TailStatus status = TailStatus::NotYet;
    Seq globalSeq = 0;
    ipc::CommandKind kind = ipc::CommandKind::New;
    ipc::Command command{};
};

class JournalTailer {
 public:
    JournalTailer(std::filesystem::path dir, Checkpoint resumeFrom) : reader_(std::move(dir)) {
        if (isRealCheckpoint(resumeFrom)) {
            reader_.seekTo(resumeFrom.createdCounter, resumeFrom.offset, resumeFrom.lastGoodSeq);
        }
    }

    JournalTailer(const JournalTailer&) = delete;
    JournalTailer& operator=(const JournalTailer&) = delete;

    // Ok -> a record, in order. NotYet -> caller should sleep and retry; either nothing new has
    // been written, or the newest record is a partial write still landing. Corrupt -> hard stop,
    // exactly as recovery treats a mid-journal sequence gap.
    TailResult next() {
        if (needsReseek_) {
            reader_.rescan();
            // JournalReader's offset_/lastGoodSeq_ never advance except on a successful Ok read
            // (neither the clean-EOF path nor the torn-tail path touches them -- both recompute
            // recordStart from the untouched offset_), so they are ALWAYS exactly "the position
            // right after the last record this reader actually read", even having survived a
            // close. currentOffset() >= SegmentHeader::kSize is the guard for "some segment has
            // been opened at least once" -- reseeking is what lets a segment that looked done at
            // clean-EOF (but is really still growing) be reopened, since JournalReader's own
            // openNextSegment() never revisits an index once nextSegmentIdx_ has passed it.
            if (reader_.currentOffset() >= SegmentHeader::kSize) {
                reader_.seekTo(reader_.currentSegmentCreatedCounter(), reader_.currentOffset(),
                               reader_.lastGoodSeq());
            }
            needsReseek_ = false;
        } else if (reader_.hasOpenSegment()) {
            reader_.refreshFileSize();
        }

        const ReadResult r = reader_.next();
        switch (r.status) {
            case ReadStatus::Ok:
                return TailResult{TailStatus::Ok, r.globalSeq, r.kind, r.command};
            case ReadStatus::Corrupt:
                return TailResult{TailStatus::Corrupt, r.globalSeq, r.kind, r.command};
            case ReadStatus::TruncatedTail:
            case ReadStatus::EndOfJournal:
            default:
                // Not corruption: either a torn in-flight write, or genuinely nothing new.
                needsReseek_ = true;
                return TailResult{TailStatus::NotYet, 0, {}, {}};
        }
    }

    Checkpoint checkpoint() const noexcept {
        return Checkpoint{reader_.currentSegmentCreatedCounter(), reader_.currentOffset(),
                          reader_.lastGoodSeq()};
    }

 private:
    static bool isRealCheckpoint(const Checkpoint& cp) noexcept { return cp.lastGoodSeq != 0; }

    JournalReader reader_;
    bool needsReseek_ = false;
};

}  // namespace velox::sequencer
