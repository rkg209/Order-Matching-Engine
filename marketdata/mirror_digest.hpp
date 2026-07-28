#pragma once

// The FR-34 oracle's other half. recovery::computeDigest() walks the real OrderBook and emits a
// StateDigest; this walks a BookMirror built purely from the market-data feed and emits the
// SAME StateDigest shape, via the SAME sequencer::OrderRecord byte layout and the SAME canonical
// order (bids best->worst, asks best->worst, FIFO within a level). Two digests comparing equal
// IS the byte-identical reconstruction claim FR-34 makes, made mechanically checkable.

#include "common/crc32.hpp"
#include "marketdata/book_mirror.hpp"
#include "recovery/state_digest.hpp"
#include "sequencer/snapshot_format.hpp"

namespace velox::marketdata {

inline recovery::StateDigest digestOf(const BookMirror& mirror) {
    recovery::StateDigest d;
    d.lastSeq = mirror.lastSeq();
    d.nextTradeId = mirror.tradeCount();
    d.restingOrders = mirror.restingOrders();

    std::vector<unsigned char> body;
    body.reserve(d.restingOrders * sequencer::OrderRecord::kSize);

    mirror.forEachOrder([&](Side side, const MirrorOrder& o) {
        sequencer::OrderRecord rec;
        rec.id = o.id;
        rec.price = o.price;
        rec.quantity = o.quantity;
        rec.remaining = o.remaining;
        rec.participant = o.participant;
        rec.seq = o.seq;
        rec.side = static_cast<std::uint8_t>(side);

        const std::size_t off = body.size();
        body.resize(off + sequencer::OrderRecord::kSize);
        rec.encode(body.data() + off);
    });

    d.bodyCrc32 = common::crc32(body.data(), body.size());
    return d;
}

}  // namespace velox::marketdata
