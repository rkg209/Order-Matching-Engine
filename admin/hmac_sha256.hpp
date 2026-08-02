#pragma once

// Spec 013 T2: RFC 2104 HMAC over admin/sha256.hpp. This is the primitive JWT HS256 verification
// (admin/jwt.hpp) is built on.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "admin/sha256.hpp"

namespace velox::admin {

inline constexpr std::size_t kSha256BlockSize = 64;

inline Sha256Digest hmacSha256(const std::uint8_t* key, std::size_t keyLen, const void* data,
                               std::size_t dataLen) noexcept {
    std::array<std::uint8_t, kSha256BlockSize> keyBlock{};
    if (keyLen > kSha256BlockSize) {
        const Sha256Digest hashed = sha256(key, keyLen);
        std::copy(hashed.begin(), hashed.end(), keyBlock.begin());
    } else {
        std::copy(key, key + keyLen, keyBlock.begin());
    }

    std::array<std::uint8_t, kSha256BlockSize> ipad{};
    std::array<std::uint8_t, kSha256BlockSize> opad{};
    for (std::size_t i = 0; i < kSha256BlockSize; ++i) {
        ipad[i] = keyBlock[i] ^ 0x36;
        opad[i] = keyBlock[i] ^ 0x5c;
    }

    std::vector<std::uint8_t> inner(kSha256BlockSize + dataLen);
    std::copy(ipad.begin(), ipad.end(), inner.begin());
    std::memcpy(inner.data() + kSha256BlockSize, data, dataLen);
    const Sha256Digest innerHash = sha256(inner.data(), inner.size());

    std::array<std::uint8_t, kSha256BlockSize + 32> outer{};
    std::copy(opad.begin(), opad.end(), outer.begin());
    std::copy(innerHash.begin(), innerHash.end(), outer.begin() + kSha256BlockSize);

    return sha256(outer.data(), outer.size());
}

inline Sha256Digest hmacSha256(const std::string& key, const std::string& data) noexcept {
    return hmacSha256(reinterpret_cast<const std::uint8_t*>(key.data()), key.size(), data.data(),
                      data.size());
}

// Constant-time comparison -- a HMAC verification that early-exits on the first mismatched byte
// leaks the MAC one byte at a time via a timing side channel. Length is not secret (both sides are
// fixed-size digests here), so it is compared with a plain `!=` up front.
inline bool constantTimeEquals(const Sha256Digest& a, const Sha256Digest& b) noexcept {
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

inline bool constantTimeEquals(const std::string& a, const std::string& b) noexcept {
    if (a.size() != b.size()) return false;
    std::uint8_t diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<std::uint8_t>(a[i] ^ b[i]);
    }
    return diff == 0;
}

}  // namespace velox::admin
