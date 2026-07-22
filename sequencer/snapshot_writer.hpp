#pragma once

// Snapshot writer (Spec 006): canonical walk of an OrderBook -> tmp file -> fsync -> rename ->
// fsync(dir) -> prune to `retention`. Off the hot path -- this runs on the shadow snapshot
// thread (sequencer/snapshot_thread.hpp), never the matching thread.
//
// Write protocol matters as much as the format: a crash between `write(tmp)` and `rename()`
// leaves only a stray .tmp file that recovery never looks at (it globs `*.snap`), so a
// half-written snapshot is either invisible or -- if the crash lands inside the rename syscall
// itself, which POSIX makes atomic -- simply the old file. There is no partially-visible state.

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/crc32.hpp"
#include "engine/order_book.hpp"
#include "platform/platform.hpp"
#include "sequencer/snapshot_format.hpp"

namespace velox::sequencer {

class SnapshotWriter {
 public:
    explicit SnapshotWriter(std::filesystem::path dir, std::size_t retention = 3)
        : dir_(std::move(dir)), retention_(retention) {
        std::filesystem::create_directories(dir_);
    }

    bool write(const OrderBook& book, Seq globalSeq, const BookConfig& cfg) {
        const std::filesystem::path tmpPath = dir_ / "snap.tmp";
        const std::filesystem::path finalPath = dir_ / finalName(globalSeq);

        std::vector<unsigned char> buf(SnapshotHeader::kSize);
        std::uint64_t orderCount = 0;

        for (const Side side : {Side::Buy, Side::Sell}) {
            const book::LevelMap& lm = book.sideView(side);
            Price p = lm.best();
            while (p != emptySentinel(side)) {
                const PriceLevel* level = lm.levelAt(p);
                for (const Order* o = level->head(); o != nullptr; o = o->next) {
                    OrderRecord rec;
                    rec.id = o->id;
                    rec.price = o->price;
                    rec.quantity = o->quantity;
                    rec.remaining = o->remaining;
                    rec.participant = o->participant;
                    rec.seq = o->seq;
                    rec.side = static_cast<std::uint8_t>(o->side);

                    const std::size_t off = buf.size();
                    buf.resize(off + OrderRecord::kSize);
                    rec.encode(buf.data() + off);
                    ++orderCount;
                }
                p = lm.nextOccupied(p);
            }
        }

        SnapshotHeader h;
        h.globalSeq = static_cast<std::uint64_t>(globalSeq);
        h.bookSeq = static_cast<std::uint64_t>(book.lastSeq());
        h.nextTradeId = static_cast<std::uint64_t>(book.tradeCount());
        h.minPrice = cfg.minPrice;
        h.maxPrice = cfg.maxPrice;
        h.tick = cfg.tick;
        h.maxOrders = static_cast<std::uint64_t>(cfg.maxOrders);
        h.stpPolicy = static_cast<std::uint8_t>(cfg.stp);
        h.orderCount = orderCount;
        h.encode(buf.data());

        const std::uint32_t crc = common::crc32(buf.data(), buf.size());
        unsigned char trailer[kSnapshotTrailerSize];
        std::memcpy(trailer, &crc, 4);

        int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0) {
            return false;
        }
        bool ok = ::write(fd, buf.data(), buf.size()) == static_cast<ssize_t>(buf.size());
        ok = ok && ::write(fd, trailer, kSnapshotTrailerSize) ==
                       static_cast<ssize_t>(kSnapshotTrailerSize);
        ok = ok && platform::fsyncFile(fd);
        ::close(fd);
        if (!ok) {
            std::error_code ec;
            std::filesystem::remove(tmpPath, ec);
            return false;
        }

        std::error_code ec;
        std::filesystem::rename(tmpPath, finalPath, ec);
        if (ec) {
            return false;
        }
        platform::fsyncDir(dir_.c_str());

        prune();
        return true;
    }

 private:
    static std::string finalName(Seq seq) {
        char b[64];
        std::snprintf(b, sizeof(b), "snap-%016llu.snap", static_cast<unsigned long long>(seq));
        return std::string(b);
    }

    // Pruned only AFTER the newer snapshot is durably renamed -- never the other way around, so
    // there is never a moment with fewer than `retention_` valid snapshots on disk unless the
    // journal genuinely has not produced that many yet.
    void prune() {
        std::vector<std::filesystem::path> snaps;
        for (const auto& e : std::filesystem::directory_iterator(dir_)) {
            if (e.is_regular_file() && e.path().extension() == ".snap") {
                snaps.push_back(e.path());
            }
        }
        std::sort(snaps.begin(), snaps.end());
        while (snaps.size() > retention_) {
            std::error_code ec;
            std::filesystem::remove(snaps.front(), ec);
            snaps.erase(snaps.begin());
        }
    }

    std::filesystem::path dir_;
    std::size_t retention_;
};

}  // namespace velox::sequencer
