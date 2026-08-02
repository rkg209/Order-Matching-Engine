// Spec 010 T7.1: sha1/base64 against known vectors, the RFC6455 worked example for the
// handshake, and frame-length encoding across the 125/126/65536 boundaries.

#include <gtest/gtest.h>

#include <iomanip>
#include <sstream>

#include "viz/base64.hpp"
#include "viz/sha1.hpp"
#include "viz/ws_server.hpp"

using namespace velox::viz;

namespace {

std::string toHex(const Sha1Digest& d) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (std::uint8_t b : d) {
        ss << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

}  // namespace

TEST(Sha1, KnownVectors) {
    EXPECT_EQ(toHex(sha1("")), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    EXPECT_EQ(toHex(sha1("abc")), "a9993e364706816aba3e25717850c26c9cd0d89d");
    EXPECT_EQ(toHex(sha1("The quick brown fox jumps over the lazy dog")),
              "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12");
}

TEST(Base64, KnownVectors) {
    auto enc = [](const std::string& s) {
        return base64Encode(reinterpret_cast<const std::uint8_t*>(s.data()), s.size());
    };
    EXPECT_EQ(enc(""), "");
    EXPECT_EQ(enc("f"), "Zg==");
    EXPECT_EQ(enc("fo"), "Zm8=");
    EXPECT_EQ(enc("foo"), "Zm9v");
    EXPECT_EQ(enc("foob"), "Zm9vYg==");
    EXPECT_EQ(enc("fooba"), "Zm9vYmE=");
    EXPECT_EQ(enc("foobar"), "Zm9vYmFy");
}

// RFC6455 section 1.3's own worked example.
TEST(WsHandshake, Rfc6455WorkedExample) {
    EXPECT_EQ(computeAcceptKey("dGhlIHNhbXBsZSBub25jZQ=="), "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
}

namespace {

std::uint64_t decodeLen(const std::vector<std::byte>& frame, std::size_t& headerBytes) {
    const std::uint8_t byte1 = static_cast<std::uint8_t>(frame[1]);
    const std::uint8_t len7 = byte1 & 0x7F;
    if (len7 <= 125) {
        headerBytes = 2;
        return len7;
    }
    if (len7 == 126) {
        headerBytes = 4;
        return (static_cast<std::uint64_t>(frame[2]) << 8) | static_cast<std::uint64_t>(frame[3]);
    }
    headerBytes = 10;
    std::uint64_t len = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        len = (len << 8) | static_cast<std::uint64_t>(frame[2 + i]);
    }
    return len;
}

}  // namespace

TEST(WsFrame, LengthBoundaries) {
    for (const std::size_t len : {0UL, 1UL, 125UL, 126UL, 65535UL, 65536UL, 70000UL}) {
        const std::string payload(len, 'x');
        const std::vector<std::byte> frame = encodeTextFrame(payload);

        EXPECT_EQ(static_cast<std::uint8_t>(frame[0]), 0x81) << "len=" << len;

        std::size_t headerBytes = 0;
        const std::uint64_t decodedLen = decodeLen(frame, headerBytes);
        EXPECT_EQ(decodedLen, len) << "len=" << len;
        EXPECT_EQ(frame.size(), headerBytes + len) << "len=" << len;

        // Payload bytes follow the header verbatim (server frames are never masked).
        for (std::size_t i = 0; i < len; ++i) {
            EXPECT_EQ(static_cast<char>(frame[headerBytes + i]), 'x');
        }
    }
}

TEST(WsFrame, LengthEncodingChoosesShortestForm) {
    std::size_t headerBytes = 0;
    decodeLen(encodeTextFrame(std::string(125, 'a')), headerBytes);
    EXPECT_EQ(headerBytes, 2u);
    decodeLen(encodeTextFrame(std::string(126, 'a')), headerBytes);
    EXPECT_EQ(headerBytes, 4u);
    decodeLen(encodeTextFrame(std::string(65535, 'a')), headerBytes);
    EXPECT_EQ(headerBytes, 4u);
    decodeLen(encodeTextFrame(std::string(65536, 'a')), headerBytes);
    EXPECT_EQ(headerBytes, 10u);
}
