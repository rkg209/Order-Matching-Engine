#pragma once

// On-disk journal segment format (Spec 006). This is the format the plan defines from scratch --
// `planning/04-database-design.md` was truncated before it got here.
//
// Everything little-endian, fixed-width, no padding beyond what is spelled out explicitly, no
// floating point. Sizes are all fixed and known so a truncated read is always detectable rather
// than ambiguous. This header only defines layout constants and encode/decode helpers over raw
// byte buffers -- it deliberately does NOT rely on compiler struct layout (padding/alignment are
// an ABI detail, not a file format), so every field is read/written by explicit offset.

#include <cstdint>
#include <cstring>

#include "common/crc32.hpp"
#include "common/types.hpp"
#include "ipc/command.hpp"

namespace velox::sequencer {

// "VXJS" read as 4 little-endian bytes.
inline constexpr std::uint32_t kJournalMagic = 0x534A5856u;
inline constexpr std::uint32_t kJournalVersion = 1;

// --- segment header: 32 bytes -----------------------------------------------------------------
//   [0:4)   u32 magic
//   [4:8)   u32 version
//   [8:16)  u64 firstSeq          -- the globalSeq of the first record in this segment
//   [16:24) u64 createdCounter    -- monotonic segment-creation counter (0, 1, 2, ...)
//   [24:28) u32 pad = 0
//   [28:32) u32 crc32(header[0..27])
struct SegmentHeader {
    static constexpr std::size_t kSize = 32;

    std::uint32_t magic = kJournalMagic;
    std::uint32_t version = kJournalVersion;
    std::uint64_t firstSeq = 0;
    std::uint64_t createdCounter = 0;

    void encode(unsigned char* out) const noexcept {
        std::memcpy(out + 0, &magic, 4);
        std::memcpy(out + 4, &version, 4);
        std::memcpy(out + 8, &firstSeq, 8);
        std::memcpy(out + 16, &createdCounter, 8);
        const std::uint32_t pad = 0;
        std::memcpy(out + 24, &pad, 4);
        const std::uint32_t crc = common::crc32(out, 28);
        std::memcpy(out + 28, &crc, 4);
    }

    // Returns false if magic/version/crc do not check out.
    bool decode(const unsigned char* in) noexcept {
        std::memcpy(&magic, in + 0, 4);
        std::memcpy(&version, in + 4, 4);
        std::memcpy(&firstSeq, in + 8, 8);
        std::memcpy(&createdCounter, in + 16, 8);
        std::uint32_t storedCrc = 0;
        std::memcpy(&storedCrc, in + 28, 4);
        const std::uint32_t crc = common::crc32(in, 28);
        return magic == kJournalMagic && version == kJournalVersion && crc == storedCrc;
    }
};

// --- record: 24 + sizeof(Command) bytes ---------------------------------------------------------
//   [0:4)    u32 payloadLen  (== sizeof(ipc::Command), always, for this spec)
//   [4:12)   u64 globalSeq
//   [12:13)  u8  commandKind
//   [13:16)  3B pad = 0
//   [16:16+payloadLen)   payload (raw byte image of ipc::Command)
//   [..:..+4) u32 crc32 over everything preceding (the fixed part + payload)
inline constexpr std::size_t kRecordFixedSize = 16;  // payloadLen+globalSeq+kind+pad
inline constexpr std::size_t kRecordCrcSize = 4;
inline constexpr std::size_t kRecordSize = kRecordFixedSize + sizeof(ipc::Command) + kRecordCrcSize;

struct JournalRecord {
    Seq globalSeq = 0;
    ipc::CommandKind kind = ipc::CommandKind::New;
    ipc::Command command{};

    void encode(unsigned char* out) const noexcept {
        const std::uint32_t payloadLen = static_cast<std::uint32_t>(sizeof(ipc::Command));
        std::memcpy(out + 0, &payloadLen, 4);
        const std::uint64_t seq = static_cast<std::uint64_t>(globalSeq);
        std::memcpy(out + 4, &seq, 8);
        const std::uint8_t kindByte = static_cast<std::uint8_t>(kind);
        std::memcpy(out + 12, &kindByte, 1);
        const unsigned char pad[3] = {0, 0, 0};
        std::memcpy(out + 13, pad, 3);
        std::memcpy(out + kRecordFixedSize, &command, sizeof(ipc::Command));
        const std::uint32_t crc = common::crc32(out, kRecordFixedSize + sizeof(ipc::Command));
        std::memcpy(out + kRecordFixedSize + sizeof(ipc::Command), &crc, 4);
    }
};

}  // namespace velox::sequencer
