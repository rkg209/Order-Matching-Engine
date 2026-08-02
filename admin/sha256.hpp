#pragma once

// Spec 013 T2: hand-rolled SHA-256 (FIPS 180-4), the block cipher underneath admin/hmac_sha256.hpp
// -- JWT HS256 verification. Same posture as viz/sha1.hpp: this repo hand-rolls its ring buffer
// and wire codec rather than reach for a dependency, and the root CMakeLists' one-command-build
// promise (see admin/API.md) means no OpenSSL/libcrypto dependency either. Unlike sha1.hpp's use
// (a handshake fingerprint), this one IS a security boundary -- so hmac_test.cpp checks it against
// the published NIST test vectors, not just "it compiles and looks textbook."
//
// Standard FIPS 180-4 SHA-256, unmodified algorithm -- see any reference implementation for the
// constants below.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace velox::admin {

using Sha256Digest = std::array<std::uint8_t, 32>;

namespace detail {

inline constexpr std::array<std::uint32_t, 64> kSha256K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline std::uint32_t rotr(std::uint32_t v, int bits) noexcept {
    return (v >> bits) | (v << (32 - bits));
}

}  // namespace detail

inline Sha256Digest sha256(const void* data, std::size_t len) noexcept {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

    const std::uint64_t bitLen = static_cast<std::uint64_t>(len) * 8;
    const std::size_t paddedLen = ((len + 8) / 64 + 1) * 64;
    std::string msg(paddedLen, '\0');
    std::memcpy(msg.data(), data, len);
    msg[len] = static_cast<char>(0x80);
    for (int i = 0; i < 8; ++i) {
        msg[paddedLen - 1 - i] = static_cast<char>((bitLen >> (8 * i)) & 0xFF);
    }

    for (std::size_t chunk = 0; chunk < paddedLen; chunk += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* b = reinterpret_cast<const std::uint8_t*>(msg.data() + chunk + i * 4);
            w[i] = (static_cast<std::uint32_t>(b[0]) << 24) |
                   (static_cast<std::uint32_t>(b[1]) << 16) |
                   (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 =
                detail::rotr(w[i - 15], 7) ^ detail::rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 =
                detail::rotr(w[i - 2], 17) ^ detail::rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                      hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t s1 = detail::rotr(e, 6) ^ detail::rotr(e, 11) ^ detail::rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ ((~e) & g);
            const std::uint32_t temp1 = hh + s1 + ch + detail::kSha256K[i] + w[i];
            const std::uint32_t s0 = detail::rotr(a, 2) ^ detail::rotr(a, 13) ^ detail::rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;

            hh = g;
            g = f;
            f = e;
            e = d + temp1;
            d = c;
            c = b;
            b = a;
            a = temp1 + temp2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    Sha256Digest out;
    for (int i = 0; i < 8; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
    }
    return out;
}

inline Sha256Digest sha256(const std::string& s) noexcept {
    return sha256(s.data(), s.size());
}

}  // namespace velox::admin
