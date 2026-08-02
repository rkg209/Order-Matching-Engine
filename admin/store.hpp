#pragma once

// Spec 013 T4: AdminStore is the filesystem read model behind every velox_adminctl route. It
// opens everything O_RDONLY (tests/structural/admin_readonly_test.cpp makes that structural, not
// just a promise) and reuses sequencer::JournalReader / the on-disk header formats Spec 006
// already defined -- it does NOT link velox_engine, velox_runtime, or velox_gateway, and it never
// touches a live OrderBook (see admin/API.md's "source":"disk" caveat).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace velox::admin {

struct InstrumentInfo {
    std::uint32_t id = 0;
    std::filesystem::path journalDir;
    std::filesystem::path snapshotDir;
};

struct EngineStatus {
    std::int64_t lastGoodSeq = 0;
    std::size_t segmentCount = 0;
    std::size_t snapshotCount = 0;
    std::int64_t newestSnapshotSeq = -1;  // -1 if no valid snapshot exists
    std::uint64_t journalBytes = 0;
    bool openSegment = false;
};

struct SegmentInfo {
    std::string fileName;
    std::int64_t firstSeq = 0;
    std::uint64_t createdCounter = 0;
    std::uint64_t bytes = 0;
    bool headerValid = false;
};

struct SnapshotInfo {
    std::string fileName;
    std::int64_t globalSeq = 0;
    std::int64_t bookSeq = 0;
    std::uint64_t orderCount = 0;
    std::uint64_t bytes = 0;
    bool crcValid = false;
};

class AdminStore {
 public:
    explicit AdminStore(std::filesystem::path journalRoot) : root_(std::move(journalRoot)) {}

    // Discovers every `shard-<id>` directory under the root (Spec 011 layout). If none exist but
    // `<root>/journal` does, the pre-Spec-011 flat single-instrument layout is reported as
    // instrument id 1 -- the same convention `apps/velox_gateway.cpp --instrument=` defaults to.
    std::vector<InstrumentInfo> listInstruments() const;

    std::optional<InstrumentInfo> findInstrument(std::uint32_t id) const;

    EngineStatus engineStatus(const InstrumentInfo& inst) const;
    std::vector<SegmentInfo> listSegments(const InstrumentInfo& inst) const;
    std::vector<SnapshotInfo> listSnapshots(const InstrumentInfo& inst) const;

 private:
    std::filesystem::path root_;
};

}  // namespace velox::admin
