#include "admin/store.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>

#include "common/crc32.hpp"
#include "sequencer/journal_format.hpp"
#include "sequencer/journal_reader.hpp"
#include "sequencer/snapshot_format.hpp"

namespace velox::admin {

namespace {

bool startsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

std::vector<InstrumentInfo> AdminStore::listInstruments() const {
    std::vector<InstrumentInfo> out;
    std::error_code ec;
    if (!std::filesystem::exists(root_, ec)) {
        return out;
    }

    for (const auto& e : std::filesystem::directory_iterator(root_, ec)) {
        if (!e.is_directory()) continue;
        const std::string name = e.path().filename().string();
        if (!startsWith(name, "shard-")) continue;
        const std::string idStr = name.substr(6);
        if (idStr.empty() || !std::all_of(idStr.begin(), idStr.end(), ::isdigit)) continue;

        InstrumentInfo info;
        info.id = static_cast<std::uint32_t>(std::stoul(idStr));
        info.journalDir = e.path() / "journal";
        info.snapshotDir = e.path() / "snapshots";
        out.push_back(std::move(info));
    }
    std::sort(out.begin(), out.end(),
              [](const InstrumentInfo& a, const InstrumentInfo& b) { return a.id < b.id; });

    if (out.empty() && std::filesystem::exists(root_ / "journal", ec)) {
        InstrumentInfo info;
        info.id = 1;
        info.journalDir = root_ / "journal";
        info.snapshotDir = root_ / "snapshots";
        out.push_back(std::move(info));
    }
    return out;
}

std::optional<InstrumentInfo> AdminStore::findInstrument(std::uint32_t id) const {
    for (auto& inst : listInstruments()) {
        if (inst.id == id) return inst;
    }
    return std::nullopt;
}

EngineStatus AdminStore::engineStatus(const InstrumentInfo& inst) const {
    EngineStatus st;

    sequencer::JournalReader reader(inst.journalDir);
    st.segmentCount = reader.segmentCount();
    for (;;) {
        const sequencer::ReadResult r = reader.next();
        if (r.status != sequencer::ReadStatus::Ok) break;
    }
    st.lastGoodSeq = static_cast<std::int64_t>(reader.lastGoodSeq());
    st.openSegment = st.segmentCount > 0;

    std::error_code ec;
    if (std::filesystem::exists(inst.journalDir, ec)) {
        for (const auto& e : std::filesystem::directory_iterator(inst.journalDir, ec)) {
            if (e.is_regular_file() && e.path().extension() == ".jnl") {
                st.journalBytes += static_cast<std::uint64_t>(e.file_size(ec));
            }
        }
    }

    const std::vector<SnapshotInfo> snaps = listSnapshots(inst);
    st.snapshotCount = snaps.size();
    for (const auto& s : snaps) {
        if (s.crcValid && s.globalSeq > st.newestSnapshotSeq) {
            st.newestSnapshotSeq = s.globalSeq;
        }
    }
    return st;
}

std::vector<SegmentInfo> AdminStore::listSegments(const InstrumentInfo& inst) const {
    std::vector<SegmentInfo> out;
    std::error_code ec;
    if (!std::filesystem::exists(inst.journalDir, ec)) {
        return out;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(inst.journalDir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".jnl") {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& p : files) {
        SegmentInfo info;
        info.fileName = p.filename().string();
        info.bytes = static_cast<std::uint64_t>(std::filesystem::file_size(p, ec));

        int fd = ::open(p.c_str(), O_RDONLY);
        if (fd >= 0) {
            unsigned char hbuf[sequencer::SegmentHeader::kSize];
            if (::read(fd, hbuf, sizeof(hbuf)) == static_cast<ssize_t>(sizeof(hbuf))) {
                sequencer::SegmentHeader h;
                info.headerValid = h.decode(hbuf);
                info.firstSeq = static_cast<std::int64_t>(h.firstSeq);
                info.createdCounter = h.createdCounter;
            }
            ::close(fd);
        }
        out.push_back(info);
    }
    return out;
}

std::vector<SnapshotInfo> AdminStore::listSnapshots(const InstrumentInfo& inst) const {
    std::vector<SnapshotInfo> out;
    std::error_code ec;
    if (!std::filesystem::exists(inst.snapshotDir, ec)) {
        return out;
    }

    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(inst.snapshotDir, ec)) {
        if (e.is_regular_file() && e.path().extension() == ".snap") {
            files.push_back(e.path());
        }
    }
    std::sort(files.begin(), files.end());

    for (const auto& p : files) {
        SnapshotInfo info;
        info.fileName = p.filename().string();

        std::ifstream f(p, std::ios::binary);
        if (!f) {
            out.push_back(info);
            continue;
        }
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
        info.bytes = bytes.size();

        if (bytes.size() >= sequencer::SnapshotHeader::kSize + sequencer::kSnapshotTrailerSize) {
            const std::size_t headerAndBodySize = bytes.size() - sequencer::kSnapshotTrailerSize;
            std::uint32_t storedCrc = 0;
            std::memcpy(&storedCrc, bytes.data() + headerAndBodySize, 4);
            const std::uint32_t crc = common::crc32(bytes.data(), headerAndBodySize);

            sequencer::SnapshotHeader h;
            h.decode(bytes.data());
            info.globalSeq = static_cast<std::int64_t>(h.globalSeq);
            info.bookSeq = static_cast<std::int64_t>(h.bookSeq);
            info.orderCount = h.orderCount;
            info.crcValid = (crc == storedCrc) && h.magic == sequencer::kSnapshotMagic &&
                            h.version == sequencer::kSnapshotVersion;
        }
        out.push_back(info);
    }
    return out;
}

}  // namespace velox::admin
