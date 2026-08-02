// Spec 013 T2: NIST SHA-256 test vectors (FIPS 180-4 Appendix B) + RFC 4231 HMAC-SHA-256 test
// cases 1-7. Hand-rolled crypto without published vectors is not crypto -- this suite is what
// makes admin/sha256.hpp and admin/hmac_sha256.hpp trustworthy before admin/jwt.hpp depends on
// them.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "admin/hmac_sha256.hpp"
#include "admin/sha256.hpp"

using namespace velox::admin;

namespace {

std::string toHex(const Sha256Digest& d) {
    static const char* hexDigits = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const std::uint8_t b : d) {
        out.push_back(hexDigits[(b >> 4) & 0xF]);
        out.push_back(hexDigits[b & 0xF]);
    }
    return out;
}

std::vector<std::uint8_t> repeated(std::uint8_t byte, std::size_t n) {
    return std::vector<std::uint8_t>(n, byte);
}

}  // namespace

// --- SHA-256 (FIPS 180-4 Appendix B) -----------------------------------------------------------

TEST(Sha256, EmptyString) {
    EXPECT_EQ(toHex(sha256("")),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256, Abc) {
    EXPECT_EQ(toHex(sha256("abc")),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256, TwoBlockMessage) {
    const std::string msg = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    EXPECT_EQ(toHex(sha256(msg)),
              "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
}

TEST(Sha256, OneMillionRepeatedA) {
    const std::string msg(1000000, 'a');
    EXPECT_EQ(toHex(sha256(msg)),
              "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// --- HMAC-SHA-256 (RFC 4231 §4.2-4.8, test cases 1-7) -------------------------------------------

TEST(HmacSha256, Case1) {
    const auto key = repeated(0x0b, 20);
    const std::string data = "Hi There";
    EXPECT_EQ(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())),
              "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7");
}

TEST(HmacSha256, Case2ShortKey) {
    const std::string key = "Jefe";
    const std::string data = "what do ya want for nothing?";
    EXPECT_EQ(toHex(hmacSha256(key, data)),
              "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(HmacSha256, Case3) {
    const auto key = repeated(0xaa, 20);
    const auto data = repeated(0xdd, 50);
    EXPECT_EQ(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())),
              "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe");
}

TEST(HmacSha256, Case4) {
    const std::vector<std::uint8_t> key = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                           0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12,
                                           0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19};
    const auto data = repeated(0xcd, 50);
    EXPECT_EQ(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())),
              "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b");
}

TEST(HmacSha256, Case5TruncatedNotTestedHere) {
    // RFC 4231's case 5 tests 128-bit truncation, which this implementation does not do (JWT
    // uses the full 256-bit HMAC). The full untruncated value is verified instead.
    const auto key = repeated(0x0c, 20);
    const std::string data = "Test With Truncation";
    EXPECT_EQ(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())),
              "a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5");
}

TEST(HmacSha256, Case6KeyLargerThanBlockSize) {
    const auto key = repeated(0xaa, 131);
    const std::string data = "Test Using Larger Than Block-Size Key - Hash Key First";
    EXPECT_EQ(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())),
              "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54");
}

TEST(HmacSha256, Case7KeyAndDataLargerThanBlockSize) {
    const auto key = repeated(0xaa, 131);
    const std::string data =
        "This is a test using a larger than block-size key and a larger than block-size data. "
        "The key needs to be hashed before being used by the HMAC algorithm.";
    EXPECT_EQ(toHex(hmacSha256(key.data(), key.size(), data.data(), data.size())),
              "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2");
}

// --- constantTimeEquals -------------------------------------------------------------------------

TEST(ConstantTimeEquals, EqualDigests) {
    const Sha256Digest d = sha256("hello");
    EXPECT_TRUE(constantTimeEquals(d, d));
}

TEST(ConstantTimeEquals, DifferentDigests) {
    EXPECT_FALSE(constantTimeEquals(sha256("hello"), sha256("world")));
}

TEST(ConstantTimeEquals, DifferentLengthStrings) {
    EXPECT_FALSE(constantTimeEquals(std::string("abc"), std::string("abcd")));
}
