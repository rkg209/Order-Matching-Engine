// Structural proof, not a code review: this translation unit includes every engine/ and book/
// header and drives a full submit/cancel/replace cycle, compiled with -fno-exceptions
// -fno-rtti. If any hot-path header requires exception support or RTTI to compile or link (a
// throw, a dynamic_cast, a typeid, a virtual base needing RTTI-backed dispatch), this file
// fails to build. That is mechanically stronger than grepping for `throw`, because it also
// catches exceptions thrown by something the hot path calls into (e.g. the standard library)
// rather than only ones written by hand.

#include "book/level_map.hpp"
#include "book/order_id_map.hpp"
#include "common/object_pool.hpp"
#include "common/types.hpp"
#include "engine/order.hpp"
#include "engine/order_book.hpp"
#include "engine/price_level.hpp"
#include "engine/trade.hpp"
#include "ipc/blocking_producer.hpp"
#include "ipc/command.hpp"
#include "ipc/multicast_ring.hpp"
#include "ipc/outbound_event.hpp"
#include "ipc/spsc_ring.hpp"

using namespace velox;

int main() {
    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1024;

    OrderBook book(cfg);
    Trade storage[8];
    TradeBuffer buf{storage, 8, 0};

    NewOrder bid{
        .id = 1, .price = 100 * kPriceScale, .quantity = 10, .participant = 1, .side = Side::Buy};
    book.submit(bid, buf);

    buf.clear();
    NewOrder ask{
        .id = 2, .price = 100 * kPriceScale, .quantity = 5, .participant = 2, .side = Side::Sell};
    book.submit(ask, buf);

    buf.clear();
    NewOrder replacement{
        .id = 3, .price = 101 * kPriceScale, .quantity = 3, .participant = 1, .side = Side::Buy};
    book.replace(1, replacement, buf);

    book.cancel(3);

    // Exercise ipc/ under the same -fno-exceptions -fno-rtti build: the ring is hot-path code
    // (runtime/ is deliberately excluded -- it legitimately owns std::thread).
    ipc::SpscRing<ipc::Command, 8> ring;
    ipc::Command cmd{.id = 1,
                     .newId = 0,
                     .price = 100 * kPriceScale,
                     .quantity = 1,
                     .participant = 1,
                     .kind = ipc::CommandKind::New,
                     .side = Side::Buy,
                     .type = OrderType::Limit};
    ipc::BlockingProducer producer(ring);
    producer.push(cmd);
    ipc::Command drained{};
    const bool popped = ring.pop(drained);

    ipc::MulticastRing<ipc::OutboundEvent, 2, 8> mcRing;
    ipc::OutboundEvent ev = ipc::tradeEvent(Trade{}, 0);
    auto* slot = mcRing.tryClaim();
    if (slot != nullptr) {
        *slot = ev;
        mcRing.publish();
    }

    return (book.restingOrders() == 0 && popped) ? 0 : 1;
}
