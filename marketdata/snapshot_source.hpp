#pragma once

// Builds a BookMirror directly from a live OrderBook (Spec 008 T6's resync path). Used only when
// marketdata::Publisher gets lapped: the ring can no longer tell it what it missed, so it falls
// back to asking the engine what the book looks like RIGHT NOW and starts fresh from there.
//
// Off the hot path -- called from the publisher thread, reading the matching thread's OrderBook
// through its existing const, noexcept, non-virtual introspection accessors (the same ones
// recovery::computeDigest() uses). Never called from engine/ or book/ itself.

#include "engine/order_book.hpp"
#include "marketdata/book_mirror.hpp"

namespace velox::marketdata {

inline void loadMirrorFromBook(const OrderBook& book, BookMirror& mirror) {
    mirror.clear();
    for (const Side side : {Side::Buy, Side::Sell}) {
        const book::LevelMap& lm = book.sideView(side);
        Price p = lm.best();
        while (p != emptySentinel(side)) {
            const PriceLevel* level = lm.levelAt(p);
            for (const Order* o = level->head(); o != nullptr; o = o->next) {
                const ipc::OrderUpdate u{o->id,        o->price,       o->quantity,
                                         o->remaining, o->participant, o->seq};
                mirror.applyOrderUpdate(ipc::UpdateAction::Rested, side, u);
            }
            p = lm.nextOccupied(p);
        }
    }
    mirror.seedCounters(book.lastSeq(), book.tradeCount());
}

}  // namespace velox::marketdata
