#pragma once

// Forward-only journal reader (Spec 006). Walks every `seg-*.jnl` file in a directory, in
// firstSeq order (guaranteed by the fixed-width %016llu filename, which sorts lexicographically
// exactly like numerically), yielding one decoded record at a time.
//
// This is used by two very different callers with the same contract: RecoveryManager replaying
// from a snapshot to bring the live book up to date, and the shadow snapshot thread replaying
// from scratch. Both need the exact same torn-tail behavior, which is why it lives here once.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include "sequencer/journal_format.hpp"

namespace velox::sequencer {

enum class ReadStatus { Ok, EndOfJournal, TruncatedTail, Corrupt };

struct ReadResult {
    ReadStatus status = ReadStatus::EndOfJournal;
    Seq globalSeq = 0;
    ipc::CommandKind kind = ipc::CommandKind::New;
    ipc::Command command{};
    std::size_t offset = 0;  // offset (within the segment it was found in) this result pertains to
};

class JournalReader {
 public:
    explicit JournalReader(std::filesystem::path dir) : dir_(std::move(dir)) {
        if (std::filesystem::exists(dir_)) {
            for (const auto& e : std::filesystem::directory_iterator(dir_)) {
                if (e.is_regular_file() && e.path().extension() == ".jnl") {
                    segments_.push_back(e.path());
                }
            }
            std::sort(segments_.begin(), segments_.end());
        }
    }

    ~JournalReader() { closeCurrent(); }

    JournalReader(const JournalReader&) = delete;
    JournalReader& operator=(const JournalReader&) = delete;

    // Reads the next record across every segment, in order. A torn tail or a sequence gap stops
    // reading for good (the caller is not expected to call next() again after a non-Ok, non-
    // EndOfJournal result) -- a torn tail is only ever legitimate on the very last segment, so
    // there is nothing meaningful to read after one anyway.
    ReadResult next() {
        for (;;) {
            if (fd_ < 0) {
                if (!openNextSegment()) {
                    return ReadResult{ReadStatus::EndOfJournal, 0, {}, {}, offset_};
                }
            }
            ReadResult r = readOneRecord();
            if (r.status == ReadStatus::EndOfJournal) {
                closeCurrent();
                continue;  // clean end of this segment; try the next one
            }
            return r;
        }
    }

    const std::filesystem::path& currentSegmentPath() const noexcept { return currentPath_; }
    std::size_t currentOffset() const noexcept { return offset_; }
    Seq currentSegmentFirstSeq() const noexcept { return currentFirstSeq_; }
    std::uint64_t currentSegmentCreatedCounter() const noexcept { return currentCreatedCounter_; }
    bool hasOpenSegment() const noexcept { return fd_ >= 0; }
    Seq lastGoodSeq() const noexcept { return lastGoodSeq_; }
    std::size_t segmentCount() const noexcept { return segments_.size(); }

 private:
    bool openNextSegment() {
        if (nextSegmentIdx_ >= segments_.size()) {
            return false;
        }
        currentPath_ = segments_[nextSegmentIdx_++];
        fd_ = ::open(currentPath_.c_str(), O_RDONLY);
        if (fd_ < 0) {
            return openNextSegment();
        }
        struct stat st{};
        ::fstat(fd_, &st);
        fileSize_ = static_cast<std::size_t>(st.st_size);

        unsigned char hbuf[SegmentHeader::kSize];
        if (fileSize_ < SegmentHeader::kSize ||
            ::read(fd_, hbuf, SegmentHeader::kSize) != static_cast<ssize_t>(SegmentHeader::kSize)) {
            closeCurrent();
            return openNextSegment();
        }
        SegmentHeader h;
        if (!h.decode(hbuf)) {
            closeCurrent();
            return openNextSegment();
        }
        currentFirstSeq_ = static_cast<Seq>(h.firstSeq);
        currentCreatedCounter_ = h.createdCounter;
        offset_ = SegmentHeader::kSize;
        return true;
    }

    ReadResult readOneRecord() {
        const std::size_t recordStart = offset_;
        if (recordStart == fileSize_) {
            return ReadResult{ReadStatus::EndOfJournal, 0, {}, {}, recordStart};
        }
        if (recordStart + kRecordFixedSize > fileSize_) {
            return torn(recordStart);
        }

        unsigned char fixed[kRecordFixedSize];
        if (::pread(fd_, fixed, kRecordFixedSize, static_cast<off_t>(recordStart)) !=
            static_cast<ssize_t>(kRecordFixedSize)) {
            return torn(recordStart);
        }
        std::uint32_t payloadLen = 0;
        std::memcpy(&payloadLen, fixed + 0, 4);
        if (payloadLen != sizeof(ipc::Command)) {
            return torn(recordStart);
        }

        const std::size_t totalSize = kRecordFixedSize + payloadLen + kRecordCrcSize;
        if (recordStart + totalSize > fileSize_) {
            return torn(recordStart);
        }

        std::vector<unsigned char> buf(totalSize);
        if (::pread(fd_, buf.data(), totalSize, static_cast<off_t>(recordStart)) !=
            static_cast<ssize_t>(totalSize)) {
            return torn(recordStart);
        }
        std::uint32_t storedCrc = 0;
        std::memcpy(&storedCrc, buf.data() + kRecordFixedSize + payloadLen, 4);
        const std::uint32_t crc = common::crc32(buf.data(), kRecordFixedSize + payloadLen);
        if (crc != storedCrc) {
            return torn(recordStart);  // CRC mismatch at the tail is a torn write, not corruption
        }

        std::uint64_t seqRaw = 0;
        std::memcpy(&seqRaw, buf.data() + 4, 8);
        const Seq seq = static_cast<Seq>(seqRaw);
        const std::uint8_t kindByte = buf[12];
        ipc::Command cmd{};
        std::memcpy(&cmd, buf.data() + kRecordFixedSize, sizeof(ipc::Command));

        if (seq != lastGoodSeq_ + 1) {
            // A gap mid-journal is corruption, not a torn tail (spec: "a gap is a hard fail").
            return ReadResult{ReadStatus::Corrupt, seq, static_cast<ipc::CommandKind>(kindByte),
                              cmd, recordStart};
        }

        offset_ = recordStart + totalSize;
        lastGoodSeq_ = seq;
        return ReadResult{ReadStatus::Ok, seq, static_cast<ipc::CommandKind>(kindByte), cmd,
                          recordStart};
    }

    ReadResult torn(std::size_t offset) noexcept {
        offset_ = offset;  // do not advance past the last known-good record
        return ReadResult{ReadStatus::TruncatedTail, 0, {}, {}, offset};
    }

    void closeCurrent() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::filesystem::path dir_;
    std::vector<std::filesystem::path> segments_;
    std::size_t nextSegmentIdx_ = 0;

    int fd_ = -1;
    std::filesystem::path currentPath_;
    std::size_t fileSize_ = 0;
    std::size_t offset_ = 0;
    Seq currentFirstSeq_ = 0;
    std::uint64_t currentCreatedCounter_ = 0;

    Seq lastGoodSeq_ = 0;
};

}  // namespace velox::sequencer
