#pragma once

// Spec 010 T4: a hand-rolled SHA-1, needed for exactly one thing -- the RFC6455 WebSocket
// handshake's Sec-WebSocket-Accept header, which is base64(sha1(key + magic guid)). The repo
// already hand-rolls its ring buffer and wire codec rather than reach for a dependency for one
// hash; this ~60-line textbook implementation matches that posture instead of pulling in OpenSSL
// or a crypto library for a single non-cryptographic-strength use (RFC6455 does not require SHA-1
// to resist attack here -- it is a handshake fingerprint, not a security boundary).
//
// Standard FIPS 180-1 SHA-1, unmodified algorithm -- see any reference implementation for the
// constants below.

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

namespace velox::viz {

using Sha1Digest = std::array<std::uint8_t, 20>;

inline std::uint32_t sha1Rotl(std::uint32_t v, int bits) noexcept {
    return (v << bits) | (v >> (32 - bits));
}

inline Sha1Digest sha1(const void* data, std::size_t len) noexcept {
    std::uint32_t h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476,
                  h4 = 0xC3D2E1F0;

    // Pad: 0x80, then zeros, then the 64-bit bit-length, to a multiple of 64 bytes.
    std::uint64_t bitLen = static_cast<std::uint64_t>(len) * 8;
    std::size_t paddedLen = ((len + 8) / 64 + 1) * 64;
    std::string msg(paddedLen, '\0');
    std::memcpy(msg.data(), data, len);
    msg[len] = static_cast<char>(0x80);
    for (int i = 0; i < 8; ++i) {
        msg[paddedLen - 1 - i] = static_cast<char>((bitLen >> (8 * i)) & 0xFF);
    }

    for (std::size_t chunk = 0; chunk < paddedLen; chunk += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const auto* b = reinterpret_cast<const std::uint8_t*>(msg.data() + chunk + i * 4);
            w[i] = (static_cast<std::uint32_t>(b[0]) << 24) |
                   (static_cast<std::uint32_t>(b[1]) << 16) |
                   (static_cast<std::uint32_t>(b[2]) << 8) | static_cast<std::uint32_t>(b[3]);
        }
        for (int i = 16; i < 80; ++i) {
            w[i] = sha1Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        }

        std::uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f, k;
            if (i < 20) {
                f = (b & c) | ((~b) & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            const std::uint32_t temp = sha1Rotl(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = sha1Rotl(b, 30);
            b = a;
            a = temp;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }

    Sha1Digest out;
    std::uint32_t h[5] = {h0, h1, h2, h3, h4};
    for (int i = 0; i < 5; ++i) {
        out[i * 4 + 0] = static_cast<std::uint8_t>((h[i] >> 24) & 0xFF);
        out[i * 4 + 1] = static_cast<std::uint8_t>((h[i] >> 16) & 0xFF);
        out[i * 4 + 2] = static_cast<std::uint8_t>((h[i] >> 8) & 0xFF);
        out[i * 4 + 3] = static_cast<std::uint8_t>(h[i] & 0xFF);
    }
    return out;
}

inline Sha1Digest sha1(const std::string& s) noexcept {
    return sha1(s.data(), s.size());
}

}  // namespace velox::viz
