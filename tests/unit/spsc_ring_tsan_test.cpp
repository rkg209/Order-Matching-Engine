// Spec 005 T0: the one place in this project where a data race is possible at all, so this is
// the one place that earns a ThreadSanitizer build (compiled with -fsanitize=thread, see
// tests/CMakeLists.txt). A smaller N than the plain unit test's: TSan instrumentation is heavy
// enough that 10M iterations would make this test slow to the point of being skipped in
// practice, and the race classes this catches do not need 10M samples to show up.

#include <gtest/gtest.h>

#include <cstdint>
#include <thread>
#include <vector>

#include "ipc/spsc_ring.hpp"

using namespace velox::ipc;

TEST(SpscRingTsan, TwoThreadStrictSequenceNoGapsNoDuplicates) {
    constexpr std::uint64_t kN = 200'000;
    SpscRing<std::uint64_t, 4096> ring;

    std::vector<std::uint64_t> received;
    received.reserve(kN);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kN; ++i) {
            while (!ring.push(i)) {
                std::this_thread::yield();
            }
        }
    });

    std::thread consumer([&] {
        std::uint64_t v = 0;
        std::uint64_t count = 0;
        while (count < kN) {
            if (ring.pop(v)) {
                received.push_back(v);
                ++count;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(received.size(), kN);
    for (std::uint64_t i = 0; i < kN; ++i) {
        ASSERT_EQ(received[i], i) << "gap or duplicate at index " << i;
    }
}
