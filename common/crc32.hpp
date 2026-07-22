#pragma once

// CRC32 (IEEE 802.3 polynomial, reflected), table-driven, header-only, no deps.
//
// Used off the hot path only: journal record integrity and snapshot integrity (Spec 006). The
// table is built once at static-init time via a constexpr generator, so there is no runtime
// table-build cost and no external table file to keep in sync.

#include <array>
#include <cstddef>
#include <cstdint>

namespace velox::common {

namespace detail {

constexpr std::array<std::uint32_t, 256> makeCrc32Table() noexcept {
    std::array<std::uint32_t, 256> table{};
    constexpr std::uint32_t kPoly = 0xEDB88320u;
    for (std::uint32_t i = 0; i < 256; ++i) {
        std::uint32_t c = i;
        for (int k = 0; k < 8; ++k) {
            c = (c & 1u) ? (kPoly ^ (c >> 1)) : (c >> 1);
        }
        table[i] = c;
    }
    return table;
}

inline constexpr std::array<std::uint32_t, 256> kCrc32Table = makeCrc32Table();

}  // namespace detail

// CRC32 over an arbitrary byte range, IEEE polynomial. crc32("123456789") == 0xCBF43926.
inline std::uint32_t crc32(const void* data, std::size_t len, std::uint32_t seed = 0) noexcept {
    const auto* bytes = static_cast<const unsigned char*>(data);
    std::uint32_t c = seed ^ 0xFFFFFFFFu;
    for (std::size_t i = 0; i < len; ++i) {
        c = detail::kCrc32Table[(c ^ bytes[i]) & 0xFFu] ^ (c >> 8);
    }
    return c ^ 0xFFFFFFFFu;
}

}  // namespace velox::common
