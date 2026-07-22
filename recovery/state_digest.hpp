#pragma once

// Canonical byte serialization of book state -> a comparable digest (Spec 006). This is what
// makes "byte-identical" testable: it walks the book using only the existing const accessors
// (never touching pool internals) and emits the same canonical byte stream the snapshot body
// uses. Two books are byte-identical iff their digests compare equal.
//
// Deliberately excluded: the object-pool free-list order. It is an allocator-internal artifact
// with no observable effect on matching, and a recovered book will legitimately differ there --
// including it here would make every recovery test flaky for a reason that isn't a bug.

#include <cstdint>
#include <vector>

#include "common/crc32.hpp"
#include "engine/order_book.hpp"
#include "sequencer/snapshot_format.hpp"

namespace velox::recovery {

struct StateDigest {
    Seq lastSeq = 0;
    Seq nextTradeId = 0;
    std::size_t restingOrders = 0;
    std::uint32_t bodyCrc32 = 0;  // crc32 over the canonical per-order byte stream

    bool operator==(const StateDigest&) const = default;
};

// Walks bids best->worst then asks best->worst, FIFO within a level -- the exact order a fresh
// restore re-establishes on insert (LevelMap::addOrder always appends to the tail), so replaying
// this stream through OrderBook::restoreResting() in order reproduces time priority bit-for-bit.
inline StateDigest computeDigest(const OrderBook& book) {
    StateDigest d;
    d.lastSeq = book.lastSeq();
    d.nextTradeId = book.tradeCount();
    d.restingOrders = book.restingOrders();

    std::vector<unsigned char> body;
    body.reserve(d.restingOrders * sequencer::OrderRecord::kSize);

    for (const Side side : {Side::Buy, Side::Sell}) {
        const book::LevelMap& lm = book.sideView(side);
        Price p = lm.best();
        while (p != emptySentinel(side)) {
            const PriceLevel* level = lm.levelAt(p);
            for (const Order* o = level->head(); o != nullptr; o = o->next) {
                sequencer::OrderRecord rec;
                rec.id = o->id;
                rec.price = o->price;
                rec.quantity = o->quantity;
                rec.remaining = o->remaining;
                rec.participant = o->participant;
                rec.seq = o->seq;
                rec.side = static_cast<std::uint8_t>(o->side);

                const std::size_t off = body.size();
                body.resize(off + sequencer::OrderRecord::kSize);
                rec.encode(body.data() + off);
            }
            p = lm.nextOccupied(p);
        }
    }

    d.bodyCrc32 = common::crc32(body.data(), body.size());
    return d;
}

}  // namespace velox::recovery
