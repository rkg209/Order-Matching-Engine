#pragma once

// Shard / ShardSet (Spec 011): bundles one instrument's entire vertical slice -- inbound ring,
// matching thread, journal, snapshot thread, sequencer -- so nothing above this header has to
// wire six objects together per instrument. This is pure composition of Specs 001-006; engine/
// and book/ are not touched by this file at all, and neither is ipc::Command's layout -- the
// shard an order belongs to is implicit in which Shard's inRing() it was pushed into, never a
// field on the command itself.
//
// Directory layout: <root>/shard-<instrumentId>/{journal,snapshots}. Journal layout is therefore
// a breaking change from the pre-Spec-011 flat <root>/{journal,snapshots} layout -- see
// apps/velox_gateway.cpp's doc comment. apps/velox_live.cpp deliberately keeps using the flat
// layout untouched (it stays single-instrument), so this file has no effect on it.

#include <cassert>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "engine/order_book.hpp"
#include "ipc/command.hpp"
#include "ipc/spsc_ring.hpp"
#include "protocol/decoder.hpp"
#include "protocol/messages.hpp"
#include "recovery/recovery_manager.hpp"
#include "runtime/matching_thread.hpp"
#include "sequencer/journal_writer.hpp"
#include "sequencer/sequencer.hpp"
#include "sequencer/snapshot_thread.hpp"

namespace velox::runtime {

// Upper bound on shard count. Not a hot-path constant (this whole file is gateway-side, off the
// hot path) -- it just keeps ShardSet's routing lookup a linear scan over an array that fits in
// one or two cache lines rather than a hash map, and N shards is bounded by physical cores anyway
// (specs/011-multi-instrument/spec.md's "Honest limits").
inline constexpr std::size_t kMaxShards = 64;

// Non-copyable, non-movable: owns rings full of atomics and a matching thread bound to `this`.
class Shard {
 public:
    using InRing = ipc::SpscRing<ipc::Command>;
    using OutRing = MatchingThread<>::OutRing;

    Shard(protocol::InstrumentId instrumentId, const BookConfig& cfg,
          std::filesystem::path shardRoot, int cpu)
        : instrumentId_(instrumentId),
          journalDir_(shardRoot / "journal"),
          snapshotDir_(shardRoot / "snapshots"),
          cfg_(cfg),
          matching_(inRing_, outRing_, cfg, cpu),
          journal_(journalDir_) {}

    ~Shard() { stop(); }

    Shard(const Shard&) = delete;
    Shard& operator=(const Shard&) = delete;
    Shard(Shard&&) = delete;
    Shard& operator=(Shard&&) = delete;

    // Recover -> resume the journal -> start the matching thread -> start the snapshot thread ->
    // construct the sequencer. Exact order apps/velox_gateway.cpp used pre-sharding (its old
    // lines 110-134), preserved verbatim because RecoveryManager::recover() must complete before
    // the matching thread starts touching the same OrderBook, and the sequencer's startSeq must
    // be the recovered lastSeq. Must be called exactly once, before any command reaches inRing().
    void recoverAndStart() {
        recovery::RecoveryManager mgr(journalDir_, snapshotDir_);
        recovery::RecoveryResult rr;
        matching_.restoreBeforeStart([&](OrderBook& b) { rr = mgr.recover(b); });
        matching_.restoreDispatchSeq(rr.lastSeq);

        if (rr.hasJournalSegment) {
            journal_.resumeFrom(rr.resumeSegmentPath, rr.resumeOffset,
                                rr.resumeSegmentCreatedCounter, rr.lastSeq);
        }

        matching_.start();

        snapshotThread_ =
            std::make_unique<sequencer::SnapshotThread>(journalDir_, snapshotDir_, cfg_);
        snapshotThread_->start();

        seqr_ = std::make_unique<sequencer::Sequencer<InRing>>(journal_, inRing_, rr.lastSeq);
        recoveredSeq_ = rr.lastSeq;
    }

    void stop() {
        if (snapshotThread_) {
            snapshotThread_->stop();
        }
        matching_.stop();
    }

    InRing& inRing() noexcept { return inRing_; }
    OutRing& outRing() noexcept { return outRing_; }
    sequencer::Sequencer<InRing>& sequencer() noexcept { return *seqr_; }
    protocol::InstrumentId instrumentId() const noexcept { return instrumentId_; }
    const MatchingThread<>& matching() const noexcept { return matching_; }
    Seq recoveredSeq() const noexcept { return recoveredSeq_; }
    const std::filesystem::path& journalDir() const noexcept { return journalDir_; }
    const std::filesystem::path& snapshotDir() const noexcept { return snapshotDir_; }
    const BookConfig& config() const noexcept { return cfg_; }

 private:
    protocol::InstrumentId instrumentId_;
    std::filesystem::path journalDir_;
    std::filesystem::path snapshotDir_;
    BookConfig cfg_;
    InRing inRing_;
    OutRing outRing_;
    MatchingThread<> matching_;
    sequencer::JournalWriter journal_;
    std::unique_ptr<sequencer::SnapshotThread> snapshotThread_;
    // Lazily constructed: the sequencer's startSeq is only known once recovery has run, and
    // Sequencer has no default constructor (it holds a JournalWriter& + Ring&).
    std::unique_ptr<sequencer::Sequencer<InRing>> seqr_;
    Seq recoveredSeq_ = 0;
};

// Owns every shard plus the instrumentId -> shard-index routing table. Lookup is a linear scan
// (see kMaxShards's doc) -- this runs on the gateway io thread, never the hot path, and an
// unordered_map here would be a hash + a pointer chase for a table that fits in a cache line.
class ShardSet {
 public:
    void addShard(protocol::InstrumentId id, const BookConfig& cfg,
                  const std::filesystem::path& root, int cpu) {
        assert(shards_.size() < kMaxShards);
        shards_.push_back(
            std::make_unique<Shard>(id, cfg, root / ("shard-" + std::to_string(id)), cpu));
    }

    void recoverAndStartAll() {
        for (auto& s : shards_) {
            s->recoverAndStart();
        }
    }

    void stopAll() {
        for (auto& s : shards_) {
            s->stop();
        }
    }

    // -1 if `id` names no configured shard.
    int indexOf(protocol::InstrumentId id) const noexcept {
        for (std::size_t i = 0; i < shards_.size(); ++i) {
            if (shards_[i]->instrumentId() == id) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    protocol::InstrumentSet instrumentSet() const {
        protocol::InstrumentSet s;
        for (const auto& shard : shards_) {
            s.add(shard->instrumentId());
        }
        return s;
    }

    std::size_t size() const noexcept { return shards_.size(); }
    Shard& operator[](std::size_t i) noexcept { return *shards_[i]; }
    const Shard& operator[](std::size_t i) const noexcept { return *shards_[i]; }

 private:
    std::vector<std::unique_ptr<Shard>> shards_;
};

}  // namespace velox::runtime
