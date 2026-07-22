#pragma once

// Backpressure contract for ring producers (Spec 005, FR-28).
//
// tryClaim() returning nullptr is the ONLY full signal a ring gives. The producer's contract
// is: spin, do not drop. A dropped order is a correctness failure in a matching engine, not a
// performance trade-off.
//
// There is no socket yet in this repo (Spec 007 adds the gateway); this is the placeholder for
// what a gateway connection will do instead of spinning forever: when its producer-side ring is
// full, it stops reading its socket, and TCP flow control pushes the backpressure out to the
// sender rather than spinning a CPU. Until that exists, this busy-spins.

#include <cstddef>

#include "platform/platform.hpp"

namespace velox::ipc {

template<typename Ring>
class BlockingProducer {
 public:
    explicit BlockingProducer(Ring& ring) : ring_(ring) {}

    // Spins on tryClaim() until a slot is available, then writes `v` into it and publishes.
    // Never returns false, never drops -- that is the entire point of this type.
    void push(const typename Ring::value_type& v) {
        for (;;) {
            auto* slot = ring_.tryClaim();
            if (slot != nullptr) {
                *slot = v;
                ring_.publish();
                return;
            }
            ++fullSpins_;
            platform::cpuPause();
        }
    }

    std::size_t fullSpins() const noexcept { return fullSpins_; }

 private:
    Ring& ring_;
    std::size_t fullSpins_ = 0;
};

}  // namespace velox::ipc
