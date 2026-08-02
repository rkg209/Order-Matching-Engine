#pragma once

// Spec 013 T1: base64 encode/decode, standard and URL-safe alphabets (RFC 4648 §4/§5).
//
// viz/base64.hpp (Spec 010 T4) already hand-rolled standard-alphabet *encode* for the WebSocket
// handshake. JWT (Spec 013) needs base64url *decode* too, which nothing in the repo had -- rather
// than a second copy, this supersedes it: viz/base64.hpp becomes a two-line forwarder.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace velox::common {

inline std::string base64Encode(const std::uint8_t* data, std::size_t len, bool urlSafe = false,
                                bool pad = true) {
    static constexpr char kStd[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static constexpr char kUrl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const char* table = urlSafe ? kUrl : kStd;

    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    std::size_t i = 0;
    while (i + 3 <= len) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8) |
                                static_cast<std::uint32_t>(data[i + 2]);
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back(table[(n >> 6) & 0x3F]);
        out.push_back(table[n & 0x3F]);
        i += 3;
    }

    const std::size_t remaining = len - i;
    if (remaining == 1) {
        const std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        if (pad) {
            out.push_back('=');
            out.push_back('=');
        }
    } else if (remaining == 2) {
        const std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
                                (static_cast<std::uint32_t>(data[i + 1]) << 8);
        out.push_back(table[(n >> 18) & 0x3F]);
        out.push_back(table[(n >> 12) & 0x3F]);
        out.push_back(table[(n >> 6) & 0x3F]);
        if (pad) {
            out.push_back('=');
        }
    }
    return out;
}

template<std::size_t N>
inline std::string base64Encode(const std::array<std::uint8_t, N>& data, bool urlSafe = false,
                                bool pad = true) {
    return base64Encode(data.data(), data.size(), urlSafe, pad);
}

inline std::string base64Encode(const std::string& data, bool urlSafe = false, bool pad = true) {
    return base64Encode(reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), urlSafe,
                        pad);
}

// Decodes `in` (standard or URL-safe alphabet, selected by `urlSafe`; padding optional -- '='
// is accepted only at the end, and only in multiples that make sense) into `out`. Returns false
// -- leaving `out` unspecified -- on any character outside the selected alphabet (aside from
// trailing '='), on a stray '=' before the end, or on a length that cannot represent whole bytes
// (i.e. exactly 1 mod 4 significant characters).
inline bool base64Decode(const std::string& in, bool urlSafe, std::vector<std::uint8_t>& out) {
    out.clear();

    auto valueOf = [urlSafe](unsigned char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (urlSafe) {
            if (c == '-') return 62;
            if (c == '_') return 63;
        } else {
            if (c == '+') return 62;
            if (c == '/') return 63;
        }
        return -1;
    };

    // Strip trailing '=' padding, if any; a '=' anywhere else is invalid.
    std::size_t end = in.size();
    while (end > 0 && in[end - 1] == '=') {
        --end;
    }
    for (std::size_t i = 0; i < end; ++i) {
        if (in[i] == '=') {
            return false;
        }
    }

    const std::size_t n = end;
    if (n % 4 == 1) {
        return false;  // cannot represent whole bytes
    }

    out.reserve((n / 4) * 3);
    std::size_t i = 0;
    while (i + 4 <= n) {
        const int v0 = valueOf(static_cast<unsigned char>(in[i]));
        const int v1 = valueOf(static_cast<unsigned char>(in[i + 1]));
        const int v2 = valueOf(static_cast<unsigned char>(in[i + 2]));
        const int v3 = valueOf(static_cast<unsigned char>(in[i + 3]));
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) return false;
        const std::uint32_t n24 =
            (static_cast<std::uint32_t>(v0) << 18) | (static_cast<std::uint32_t>(v1) << 12) |
            (static_cast<std::uint32_t>(v2) << 6) | static_cast<std::uint32_t>(v3);
        out.push_back(static_cast<std::uint8_t>((n24 >> 16) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n24 >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(n24 & 0xFF));
        i += 4;
    }

    const std::size_t remaining = n - i;
    if (remaining == 2) {
        const int v0 = valueOf(static_cast<unsigned char>(in[i]));
        const int v1 = valueOf(static_cast<unsigned char>(in[i + 1]));
        if (v0 < 0 || v1 < 0) return false;
        const std::uint32_t n12 =
            (static_cast<std::uint32_t>(v0) << 6) | static_cast<std::uint32_t>(v1);
        out.push_back(static_cast<std::uint8_t>((n12 >> 4) & 0xFF));
    } else if (remaining == 3) {
        const int v0 = valueOf(static_cast<unsigned char>(in[i]));
        const int v1 = valueOf(static_cast<unsigned char>(in[i + 1]));
        const int v2 = valueOf(static_cast<unsigned char>(in[i + 2]));
        if (v0 < 0 || v1 < 0 || v2 < 0) return false;
        const std::uint32_t n18 = (static_cast<std::uint32_t>(v0) << 12) |
                                  (static_cast<std::uint32_t>(v1) << 6) |
                                  static_cast<std::uint32_t>(v2);
        out.push_back(static_cast<std::uint8_t>((n18 >> 10) & 0xFF));
        out.push_back(static_cast<std::uint8_t>((n18 >> 2) & 0xFF));
    }
    return true;
}

inline bool base64Decode(const std::string& in, bool urlSafe, std::string& out) {
    std::vector<std::uint8_t> bytes;
    if (!base64Decode(in, urlSafe, bytes)) return false;
    out.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    return true;
}

}  // namespace velox::common
