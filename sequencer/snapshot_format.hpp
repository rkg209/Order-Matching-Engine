#pragma once

// On-disk snapshot format (Spec 006). `planning/04-database-design.md` was truncated before
// §8 (the snapshot binary format); this header is where that gets designed instead.
//
// The book is NOT memory-dumped: `Order` holds raw `Order*`/`PriceLevel*` links and `LevelMap`
// holds heap arrays, so a raw image would be meaningless across processes. Instead this stores
// logical state in CANONICAL order -- bids best-to-worst, then asks best-to-worst, FIFO within
// a level -- which restores to an observably identical book by straight re-insertion (see
// recovery/state_digest.hpp, which uses the exact same walk and the exact same record layout).
//
// Every resting order is, by construction, a LIMIT order: MARKET/IOC never rest and FOK is
// all-or-nothing (see engine/order_book.hpp's OrderType doc comment), so there is no `type`
// field to persist per-order -- restore always re-rests as LIMIT.

#include <cstdint>
#include <cstring>

namespace velox::sequencer {

inline constexpr std::uint32_t kSnapshotMagic = 0x4E535856u;  // "VXSN"
inline constexpr std::uint32_t kSnapshotVersion = 1;

// --- header ---------------------------------------------------------------------------------
//   [0:4)    u32 magic
//   [4:8)    u32 version
//   [8:16)   u64 globalSeq      -- the sequencer seq this snapshot was taken at
//   [16:24)  u64 bookSeq        -- OrderBook::lastSeq() at that point
//   [24:32)  u64 nextTradeId    -- OrderBook::tradeCount() at that point
//   [32:40)  i64 minPrice
//   [40:48)  i64 maxPrice
//   [48:56)  i64 tick
//   [56:64)  u64 maxOrders
//   [64:65)  u8  stpPolicy
//   [65:72)  7B pad = 0
//   [72:80)  u64 orderCount
struct SnapshotHeader {
    static constexpr std::size_t kSize = 80;

    std::uint32_t magic = kSnapshotMagic;
    std::uint32_t version = kSnapshotVersion;
    std::uint64_t globalSeq = 0;
    std::uint64_t bookSeq = 0;
    std::uint64_t nextTradeId = 0;
    std::int64_t minPrice = 0;
    std::int64_t maxPrice = 0;
    std::int64_t tick = 0;
    std::uint64_t maxOrders = 0;
    std::uint8_t stpPolicy = 0;
    std::uint64_t orderCount = 0;

    void encode(unsigned char* out) const noexcept {
        std::memcpy(out + 0, &magic, 4);
        std::memcpy(out + 4, &version, 4);
        std::memcpy(out + 8, &globalSeq, 8);
        std::memcpy(out + 16, &bookSeq, 8);
        std::memcpy(out + 24, &nextTradeId, 8);
        std::memcpy(out + 32, &minPrice, 8);
        std::memcpy(out + 40, &maxPrice, 8);
        std::memcpy(out + 48, &tick, 8);
        std::memcpy(out + 56, &maxOrders, 8);
        std::memcpy(out + 64, &stpPolicy, 1);
        const unsigned char pad[7] = {0, 0, 0, 0, 0, 0, 0};
        std::memcpy(out + 65, pad, 7);
        std::memcpy(out + 72, &orderCount, 8);
    }

    void decode(const unsigned char* in) noexcept {
        std::memcpy(&magic, in + 0, 4);
        std::memcpy(&version, in + 4, 4);
        std::memcpy(&globalSeq, in + 8, 8);
        std::memcpy(&bookSeq, in + 16, 8);
        std::memcpy(&nextTradeId, in + 24, 8);
        std::memcpy(&minPrice, in + 32, 8);
        std::memcpy(&maxPrice, in + 40, 8);
        std::memcpy(&tick, in + 48, 8);
        std::memcpy(&maxOrders, in + 56, 8);
        std::memcpy(&stpPolicy, in + 64, 1);
        std::memcpy(&orderCount, in + 72, 8);
    }
};

// --- order record: 56 bytes ------------------------------------------------------------------
struct OrderRecord {
    static constexpr std::size_t kSize = 56;

    std::int64_t id = 0;
    std::int64_t price = 0;
    std::int64_t quantity = 0;
    std::int64_t remaining = 0;
    std::int64_t participant = 0;
    std::int64_t seq = 0;
    std::uint8_t side = 0;

    void encode(unsigned char* out) const noexcept {
        std::memcpy(out + 0, &id, 8);
        std::memcpy(out + 8, &price, 8);
        std::memcpy(out + 16, &quantity, 8);
        std::memcpy(out + 24, &remaining, 8);
        std::memcpy(out + 32, &participant, 8);
        std::memcpy(out + 40, &seq, 8);
        std::memcpy(out + 48, &side, 1);
        const unsigned char pad[7] = {0, 0, 0, 0, 0, 0, 0};
        std::memcpy(out + 49, pad, 7);
    }

    void decode(const unsigned char* in) noexcept {
        std::memcpy(&id, in + 0, 8);
        std::memcpy(&price, in + 8, 8);
        std::memcpy(&quantity, in + 16, 8);
        std::memcpy(&remaining, in + 24, 8);
        std::memcpy(&participant, in + 32, 8);
        std::memcpy(&seq, in + 40, 8);
        std::memcpy(&side, in + 48, 1);
    }
};

inline constexpr std::size_t kSnapshotTrailerSize = 4;  // u32 crc32 over header+body

}  // namespace velox::sequencer
