#pragma once

// Explicit little-endian byte load/store over std::byte buffers (Spec 007 plan: "never
// reinterpret_cast of a packed struct" -- struct punning across a network boundary is UB,
// alignment-fragile on any platform where the input buffer isn't naturally aligned, and silent
// about endianness. This file is the only place that reasons about byte order.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace velox::protocol::wire {

inline void putU8(std::byte* dst, std::uint8_t v) noexcept {
    dst[0] = std::byte{v};
}

inline std::uint8_t getU8(const std::byte* src) noexcept {
    return std::to_integer<std::uint8_t>(src[0]);
}

inline void putU32(std::byte* dst, std::uint32_t v) noexcept {
    for (int i = 0; i < 4; ++i) {
        dst[i] = std::byte{static_cast<std::uint8_t>(v >> (8 * i))};
    }
}

inline std::uint32_t getU32(const std::byte* src) noexcept {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(src[i])) << (8 * i);
    }
    return v;
}

inline void putU64(std::byte* dst, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        dst[i] = std::byte{static_cast<std::uint8_t>(v >> (8 * i))};
    }
}

inline std::uint64_t getU64(const std::byte* src) noexcept {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) {
        v |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(src[i])) << (8 * i);
    }
    return v;
}

inline void putI64(std::byte* dst, std::int64_t v) noexcept {
    putU64(dst, static_cast<std::uint64_t>(v));
}

inline std::int64_t getI64(const std::byte* src) noexcept {
    return static_cast<std::int64_t>(getU64(src));
}

}  // namespace velox::protocol::wire
