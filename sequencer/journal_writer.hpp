#pragma once

// Segmented, append-only journal writer (Spec 006). Off the hot path entirely -- this runs on
// the sequencer thread, never the matching thread -- so it is free to allocate, use
// std::filesystem, and block on I/O.
//
// Durability policy (spec "Conflict 2"): PerRecord fsyncs after every single append -- the
// durable-throughput ceiling this implies is measured and reported separately from the bench-mode
// headline (see apps/velox_live.cpp). Group(N) fsyncs every N records instead, trading a bounded
// window of loss-on-power-failure for higher durable throughput; that weaker guarantee is stated
// in the durable-throughput output, never buried.

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include "platform/platform.hpp"
#include "sequencer/journal_format.hpp"

namespace velox::sequencer {

enum class FsyncPolicy { PerRecord, Group };

class JournalWriter {
 public:
    JournalWriter(std::filesystem::path dir, std::size_t rollBytes = 256u * 1024 * 1024,
                  FsyncPolicy policy = FsyncPolicy::PerRecord, std::size_t groupSize = 1)
        : dir_(std::move(dir)),
          rollBytes_(rollBytes),
          policy_(policy),
          groupSize_(groupSize == 0 ? 1 : groupSize) {
        std::filesystem::create_directories(dir_);
    }

    ~JournalWriter() { close(); }

    JournalWriter(const JournalWriter&) = delete;
    JournalWriter& operator=(const JournalWriter&) = delete;

    // Continue an existing journal after recovery: reopen the last segment, truncate it to
    // `validOffset` (the torn-tail boundary a JournalReader found, or the file's own clean size
    // if it was not torn), and resume appending from there. This is "the writer truncates the
    // segment to that offset and continues" (spec, torn-tail handling) -- there is no separate
    // repair tool.
    bool resumeFrom(const std::filesystem::path& segmentPath, std::size_t validOffset,
                    std::uint64_t createdCounter, Seq lastSeq) {
        close();
        fd_ = ::open(segmentPath.c_str(), O_RDWR);
        if (fd_ < 0) {
            return false;
        }
        if (::ftruncate(fd_, static_cast<off_t>(validOffset)) != 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        if (::lseek(fd_, static_cast<off_t>(validOffset), SEEK_SET) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        currentPath_ = segmentPath;
        bytesInSegment_ = validOffset;
        createdCounter_ = createdCounter + 1;  // the NEXT segment created will use this number
        lastSeq_ = lastSeq;
        return true;
    }

    // Appends one record and, per policy, fsyncs before returning. Returns false on I/O error --
    // the caller must treat that as a durability failure, never a silent ack.
    bool append(Seq globalSeq, ipc::CommandKind kind, const ipc::Command& cmd) {
        if (fd_ < 0) {
            if (!openSegment(globalSeq)) {
                return false;
            }
        } else if (bytesInSegment_ + kRecordSize > rollBytes_) {
            if (!rollSegment(globalSeq)) {
                return false;
            }
        }

        JournalRecord rec{globalSeq, kind, cmd};
        unsigned char buf[kRecordSize];
        rec.encode(buf);
        const ssize_t n = ::write(fd_, buf, kRecordSize);
        if (n != static_cast<ssize_t>(kRecordSize)) {
            return false;
        }
        bytesInSegment_ += kRecordSize;
        lastSeq_ = globalSeq;
        ++pendingUnsynced_;

        if (policy_ == FsyncPolicy::PerRecord || pendingUnsynced_ >= groupSize_) {
            return sync();
        }
        return true;
    }

    // Forces a durable flush now. Called internally per the fsync policy, and by callers at
    // shutdown to make sure a partial group is not left unsynced.
    bool sync() {
        if (fd_ < 0) {
            return true;
        }
        if (!platform::fsyncFile(fd_)) {
            return false;
        }
        pendingUnsynced_ = 0;
        return true;
    }

    Seq lastSeq() const noexcept { return lastSeq_; }
    const std::filesystem::path& currentSegmentPath() const noexcept { return currentPath_; }
    FsyncPolicy policy() const noexcept { return policy_; }

 private:
    static std::string segmentName(Seq firstSeq) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "seg-%016llu.jnl",
                      static_cast<unsigned long long>(firstSeq));
        return std::string(buf);
    }

    bool openSegment(Seq firstSeq) {
        currentPath_ = dir_ / segmentName(firstSeq);
        fd_ = ::open(currentPath_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd_ < 0) {
            return false;
        }
        SegmentHeader h;
        h.firstSeq = static_cast<std::uint64_t>(firstSeq);
        h.createdCounter = createdCounter_++;
        unsigned char hbuf[SegmentHeader::kSize];
        h.encode(hbuf);
        if (::write(fd_, hbuf, SegmentHeader::kSize) !=
            static_cast<ssize_t>(SegmentHeader::kSize)) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        bytesInSegment_ = SegmentHeader::kSize;
        return platform::fsyncFile(fd_);  // the header itself must survive a crash too
    }

    bool rollSegment(Seq nextFirstSeq) {
        if (!sync()) {
            return false;
        }
        ::close(fd_);
        fd_ = -1;
        return openSegment(nextFirstSeq);
    }

    void close() {
        if (fd_ >= 0) {
            sync();
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::filesystem::path dir_;
    std::size_t rollBytes_;
    FsyncPolicy policy_;
    std::size_t groupSize_;

    int fd_ = -1;
    std::filesystem::path currentPath_;
    std::size_t bytesInSegment_ = 0;
    std::size_t pendingUnsynced_ = 0;
    std::uint64_t createdCounter_ = 0;
    Seq lastSeq_ = 0;
};

}  // namespace velox::sequencer
