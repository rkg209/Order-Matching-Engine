#pragma once

// Snapshot reader (Spec 006): newest-valid-snapshot selection with CRC validation.
//
// Recovery walks snapshots newest-first and takes the first that validates -- a half-written or
// bit-flipped snapshot is silently skipped in favor of the next-newest valid one, which is what
// CRC32 + atomic rename buy: a bad snapshot is detectable, never loaded, and never fatal as long
// as an older valid one still exists.

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <vector>

#include "common/crc32.hpp"
#include "sequencer/snapshot_format.hpp"

namespace velox::sequencer {

struct LoadedSnapshot {
    SnapshotHeader header;
    std::vector<OrderRecord> orders;
};

class SnapshotReader {
 public:
    explicit SnapshotReader(std::filesystem::path dir) : dir_(std::move(dir)) {}

    std::optional<LoadedSnapshot> loadNewestValid() const {
        std::vector<std::filesystem::path> snaps;
        if (std::filesystem::exists(dir_)) {
            for (const auto& e : std::filesystem::directory_iterator(dir_)) {
                if (e.is_regular_file() && e.path().extension() == ".snap") {
                    snaps.push_back(e.path());
                }
            }
        }
        std::sort(snaps.rbegin(), snaps.rend());  // newest (highest seq) first

        for (const auto& p : snaps) {
            if (auto loaded = tryLoad(p)) {
                return loaded;
            }
        }
        return std::nullopt;
    }

 private:
    std::optional<LoadedSnapshot> tryLoad(const std::filesystem::path& p) const {
        std::ifstream f(p, std::ios::binary);
        if (!f) {
            return std::nullopt;
        }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
        if (bytes.size() < SnapshotHeader::kSize + kSnapshotTrailerSize) {
            return std::nullopt;
        }

        const std::size_t headerAndBodySize = bytes.size() - kSnapshotTrailerSize;
        std::uint32_t storedCrc = 0;
        std::memcpy(&storedCrc, bytes.data() + headerAndBodySize, 4);
        const std::uint32_t crc = common::crc32(bytes.data(), headerAndBodySize);
        if (crc != storedCrc) {
            return std::nullopt;
        }

        SnapshotHeader h;
        h.decode(bytes.data());
        if (h.magic != kSnapshotMagic || h.version != kSnapshotVersion) {
            return std::nullopt;
        }

        const std::size_t expectedBodySize =
            static_cast<std::size_t>(h.orderCount) * OrderRecord::kSize;
        if (headerAndBodySize != SnapshotHeader::kSize + expectedBodySize) {
            return std::nullopt;
        }

        LoadedSnapshot result;
        result.header = h;
        result.orders.resize(h.orderCount);
        for (std::uint64_t i = 0; i < h.orderCount; ++i) {
            result.orders[i].decode(bytes.data() + SnapshotHeader::kSize + i * OrderRecord::kSize);
        }
        return result;
    }

    std::filesystem::path dir_;
};

}  // namespace velox::sequencer
