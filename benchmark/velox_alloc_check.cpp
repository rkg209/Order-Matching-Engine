// Allocation check (NFR-9, NFR-12).
//
// Overrides global operator new/delete with counting versions, warms up to steady state, ZEROES
// the counters, then drives orders through the matching path. Anything counted after the reset
// is an allocation on the hot path.
//
// The reset is the entire trick. Startup allocation (pools, level arrays) is permitted and
// expected -- the constitution forbids allocation PER OPERATION at steady state, not the
// existence of memory. Counting from process start would measure the wrong thing and report a
// scary number that means nothing.

#include <cstdio>
#include <cstdlib>
#include <new>
#include <vector>

#include "engine/order_book.hpp"

namespace {

std::size_t g_allocCount = 0;
std::size_t g_allocBytes = 0;
bool g_counting = false;

}  // namespace

void* operator new(std::size_t n) {
    if (g_counting) {
        ++g_allocCount;
        g_allocBytes += n;
    }
    void* p = std::malloc(n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n) {
    return operator new(n);
}

// Over-aligned allocations (e.g. a type with alignas() past __STDCPP_DEFAULT_NEW_ALIGNMENT__)
// go through these overloads instead of the plain ones above. Without them, an over-aligned
// hot-path allocation would silently escape the counter entirely -- a proof hole, not a fix.
void* operator new(std::size_t n, std::align_val_t al) {
    if (g_counting) {
        ++g_allocCount;
        g_allocBytes += n;
    }
    void* p = std::aligned_alloc(static_cast<std::size_t>(al), n);
    if (p == nullptr) throw std::bad_alloc();
    return p;
}

void* operator new[](std::size_t n, std::align_val_t al) {
    return operator new(n, al);
}

void operator delete(void* p) noexcept {
    std::free(p);
}
void operator delete[](void* p) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete(void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept {
    std::free(p);
}

using namespace velox;

namespace {

// Widened workload (Spec 004 T1): the original version only ever called submit() with LIMIT
// orders, so cancel(), replace(), and MARKET/IOC/FOK -- the whole of Spec 002 -- were unproven
// at 0 bytes/op. This drives all of them at steady state, in a 6-op cycle:
//
//   0: LIMIT rest        1: LIMIT cross (fills op 0's resting order -- exercises matchInto/trades)
//   2: cancel()          3: replace() (cancel+submit, on a churn pool of OTM resting orders)
//   4: MARKET            5: IOC / FOK alternately (both consume from deep OTM reservoirs)
//
// Every op is paired with its own inverse within the cycle, so pool occupancy is flat forever:
// nothing here can exhaust the pool or run the book out of liquidity.
struct Workload {
    static constexpr Price kMid = 100 * kPriceScale;
    static constexpr Price kTick = kPriceScale / 100;
    static constexpr std::size_t kChurn = 256;  // cancel/replace target pool, each side
    static constexpr Quantity kDeepQty =
        100'000'000;  // reservoir for MARKET/IOC/FOK; never runs out

    explicit Workload(OrderBook& b) : book(b) {
        // Deep OTM reservoirs that MARKET/IOC/FOK nibble from, 1 unit at a time, forever.
        buf.clear();
        book.submit(NewOrder{.id = nextId++,
                             .price = kMid + 80 * kTick,
                             .quantity = kDeepQty,
                             .participant = 100,
                             .side = Side::Sell},
                    buf);
        buf.clear();
        book.submit(NewOrder{.id = nextId++,
                             .price = kMid - 80 * kTick,
                             .quantity = kDeepQty,
                             .participant = 101,
                             .side = Side::Buy},
                    buf);

        // Churn pool: far out-of-the-money resting limit orders that cancel()/replace() target.
        // Spread across distinct OTM prices so each sits alone at its own level.
        churnIds.resize(kChurn);
        for (std::size_t k = 0; k < kChurn; ++k) {
            const bool buy = (k % 2) == 0;
            buf.clear();
            NewOrder o{
                .id = nextId,
                .price = buy ? kMid - (90 + static_cast<Price>(k)) * kTick
                             : kMid + (90 + static_cast<Price>(k)) * kTick,
                .quantity = 10,
                .participant = 102,
                .side = buy ? Side::Buy : Side::Sell,
            };
            book.submit(o, buf);
            churnIds[k] = nextId++;
        }
    }

    void step(std::size_t i) {
        switch (i % 6) {
            case 0: {
                buf.clear();
                book.submit(NewOrder{.id = nextId++,
                                     .price = kMid - kTick,
                                     .quantity = 10,
                                     .participant = 2,
                                     .side = Side::Buy},
                            buf);
                break;
            }
            case 1: {
                buf.clear();
                book.submit(NewOrder{.id = nextId++,
                                     .price = kMid - kTick,
                                     .quantity = 10,
                                     .participant = 3,
                                     .side = Side::Sell},
                            buf);
                break;
            }
            case 2: {
                std::size_t& c = cancelCursor;
                book.cancel(churnIds[c]);
                const bool buy = (c % 2) == 0;
                buf.clear();
                NewOrder o{
                    .id = nextId,
                    .price = buy ? kMid - (90 + static_cast<Price>(c)) * kTick
                                 : kMid + (90 + static_cast<Price>(c)) * kTick,
                    .quantity = 10,
                    .participant = 102,
                    .side = buy ? Side::Buy : Side::Sell,
                };
                book.submit(o, buf);
                churnIds[c] = nextId++;
                c = (c + 1) % kChurn;
                break;
            }
            case 3: {
                std::size_t& c = replaceCursor;
                const bool buy = (c % 2) == 0;
                buf.clear();
                NewOrder fresh{
                    .id = nextId,
                    .price = buy ? kMid - (90 + static_cast<Price>(c)) * kTick
                                 : kMid + (90 + static_cast<Price>(c)) * kTick,
                    .quantity = 10,
                    .participant = 102,
                    .side = buy ? Side::Buy : Side::Sell,
                };
                book.replace(churnIds[c], fresh, buf);
                churnIds[c] = nextId++;
                c = (c + 1) % kChurn;
                break;
            }
            case 4: {
                buf.clear();
                book.submit(NewOrder{.id = nextId++,
                                     .price = 0,  // ignored for MARKET
                                     .quantity = 1,
                                     .participant = 200,
                                     .side = Side::Buy,
                                     .type = OrderType::Market},
                            buf);
                break;
            }
            case 5: {
                buf.clear();
                if ((i / 6) % 2 == 0) {
                    book.submit(NewOrder{.id = nextId++,
                                         .price = kMid - 80 * kTick,
                                         .quantity = 1,
                                         .participant = 201,
                                         .side = Side::Sell,
                                         .type = OrderType::Ioc},
                                buf);
                } else {
                    book.submit(NewOrder{.id = nextId++,
                                         .price = kMid + 80 * kTick,
                                         .quantity = 1,
                                         .participant = 202,
                                         .side = Side::Buy,
                                         .type = OrderType::Fok},
                                buf);
                }
                break;
            }
        }
    }

    OrderBook& book;
    Trade storage[64];
    TradeBuffer buf{storage, 64, 0};
    OrderId nextId = 1;
    std::vector<OrderId> churnIds;
    std::size_t cancelCursor = 0;
    std::size_t replaceCursor = 0;
};

}  // namespace

int main() {
    constexpr std::size_t kWarmup = 50'000;
    constexpr std::size_t kMeasured = 200'000;

    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kPriceScale / 100;
    cfg.maxOrders = 1u << 20;

    OrderBook book(cfg);
    Workload workload(book);

    // --- warmup: reach steady state -----------------------------------------------------
    // Fill the pools, touch the pages, drive every op path once. The first Order taken from a
    // fresh pool touches a cold page; that is a startup cost, not a matching cost.
    for (std::size_t i = 0; i < kWarmup; ++i) {
        workload.step(i);
    }

    // --- measure ------------------------------------------------------------------------
    g_allocCount = 0;
    g_allocBytes = 0;
    g_counting = true;

    for (std::size_t i = 0; i < kMeasured; ++i) {
        workload.step(i);
    }

    g_counting = false;

    const double bytesPerOp = static_cast<double>(g_allocBytes) / kMeasured;
    const double allocsPerOp = static_cast<double>(g_allocCount) / kMeasured;

    std::printf("velox alloc-check\n");
    std::printf("  operations:      %zu (after %zu warmup)\n", kMeasured, kWarmup);
    std::printf("  allocations:     %zu\n", g_allocCount);
    std::printf("  bytes:           %zu\n", g_allocBytes);
    std::printf("  allocs/op:       %.6f\n", allocsPerOp);
    std::printf("  bytes/op:        %.6f\n", bytesPerOp);
    std::printf("  budget:          0 bytes/op, 0 allocs/op (NFR-9)\n");

    if (g_allocCount == 0) {
        std::printf("  RESULT:          PASS -- hot path allocates nothing.\n");
        return 0;
    }

    std::printf("  RESULT:          FAIL -- the hot path allocates.\n");
    std::printf("                   The fix is never 'allocate less'; it is 'allocate never'.\n");
    return 1;
}
