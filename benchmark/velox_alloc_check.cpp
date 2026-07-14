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

using namespace velox;

int main() {
    constexpr std::size_t kWarmup = 50'000;
    constexpr std::size_t kMeasured = 200'000;
    constexpr Price kMid = 100 * kPriceScale;
    constexpr Price kTick = kPriceScale / 100;

    BookConfig cfg;
    cfg.minPrice = 1 * kPriceScale;
    cfg.maxPrice = 200 * kPriceScale;
    cfg.tick = kTick;
    cfg.maxOrders = 1u << 20;

    OrderBook book(cfg);
    Trade storage[64];
    TradeBuffer buf{storage, 64, 0};

    OrderId id = 1;

    // --- warmup: reach steady state -----------------------------------------------------
    // Fill the pools, touch the pages, populate the book. The first Order taken from a fresh
    // pool touches a cold page; that is a startup cost, not a matching cost.
    for (std::size_t i = 0; i < kWarmup; ++i) {
        buf.clear();
        const bool buy = (i % 2) == 0;
        NewOrder o{
            .id = id++,
            .price = buy ? kMid - kTick * static_cast<Price>(1 + (i % 20))
                         : kMid + kTick * static_cast<Price>(1 + (i % 20)),
            .quantity = 10,
            .participant = 1,
            .side = buy ? Side::Buy : Side::Sell,
        };
        book.submit(o, buf);
    }

    // --- measure ------------------------------------------------------------------------
    g_allocCount = 0;
    g_allocBytes = 0;
    g_counting = true;

    for (std::size_t i = 0; i < kMeasured; ++i) {
        buf.clear();
        const bool buy = (i % 2) == 0;
        NewOrder o{
            .id = id++,
            .price = buy ? kMid - kTick * static_cast<Price>(1 + (i % 20))
                         : kMid + kTick * static_cast<Price>(1 + (i % 20)),
            .quantity = 10,
            .participant = 1,
            .side = buy ? Side::Buy : Side::Sell,
        };
        book.submit(o, buf);
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
