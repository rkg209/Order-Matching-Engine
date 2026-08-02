// Spec 013 T2: RFC 4648 §10 test vectors for standard base64, plus URL-safe-alphabet and
// round-trip / invalid-input coverage that RFC 4648 doesn't itself supply worked examples for.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "common/base64.hpp"

using namespace velox::common;

TEST(Base64, Rfc4648Vectors) {
    EXPECT_EQ(base64Encode(""), "");
    EXPECT_EQ(base64Encode("f"), "Zg==");
    EXPECT_EQ(base64Encode("fo"), "Zm8=");
    EXPECT_EQ(base64Encode("foo"), "Zm9v");
    EXPECT_EQ(base64Encode("foob"), "Zm9vYg==");
    EXPECT_EQ(base64Encode("fooba"), "Zm9vYmE=");
    EXPECT_EQ(base64Encode("foobar"), "Zm9vYmFy");
}

TEST(Base64, DecodeRfc4648Vectors) {
    std::string out;
    ASSERT_TRUE(base64Decode("", false, out));
    EXPECT_EQ(out, "");
    ASSERT_TRUE(base64Decode("Zg==", false, out));
    EXPECT_EQ(out, "f");
    ASSERT_TRUE(base64Decode("Zm8=", false, out));
    EXPECT_EQ(out, "fo");
    ASSERT_TRUE(base64Decode("Zm9v", false, out));
    EXPECT_EQ(out, "foo");
    ASSERT_TRUE(base64Decode("Zm9vYg==", false, out));
    EXPECT_EQ(out, "foob");
    ASSERT_TRUE(base64Decode("Zm9vYmE=", false, out));
    EXPECT_EQ(out, "fooba");
    ASSERT_TRUE(base64Decode("Zm9vYmFy", false, out));
    EXPECT_EQ(out, "foobar");
}

TEST(Base64, DecodeWithoutPaddingSucceeds) {
    // JWT segments are base64url with padding stripped -- decode must accept that.
    std::string out;
    ASSERT_TRUE(base64Decode("Zg", false, out));
    EXPECT_EQ(out, "f");
    ASSERT_TRUE(base64Decode("Zm8", false, out));
    EXPECT_EQ(out, "fo");
}

TEST(Base64, UrlSafeAlphabetRoundTrip) {
    // Bytes chosen so the standard alphabet would emit '+' and '/' (0xfb 0xff -> "+/8=" area).
    const std::vector<std::uint8_t> data = {0xfb, 0xff, 0xfe};
    const std::string encStd = base64Encode(data.data(), data.size(), /*urlSafe=*/false);
    const std::string encUrl = base64Encode(data.data(), data.size(), /*urlSafe=*/true);
    EXPECT_NE(encStd.find('+'), std::string::npos);
    EXPECT_EQ(encUrl.find('+'), std::string::npos);
    EXPECT_EQ(encUrl.find('/'), std::string::npos);

    std::vector<std::uint8_t> decoded;
    ASSERT_TRUE(base64Decode(encUrl, /*urlSafe=*/true, decoded));
    EXPECT_EQ(decoded, data);
}

TEST(Base64, NoPaddingUrlSafeRoundTrip) {
    for (std::size_t len = 0; len <= 8; ++len) {
        std::vector<std::uint8_t> data(len);
        for (std::size_t i = 0; i < len; ++i) data[i] = static_cast<std::uint8_t>(i * 37 + 1);
        const std::string enc = base64Encode(data.data(), data.size(), /*urlSafe=*/true,
                                             /*pad=*/false);
        EXPECT_EQ(enc.find('='), std::string::npos);
        std::vector<std::uint8_t> decoded;
        ASSERT_TRUE(base64Decode(enc, /*urlSafe=*/true, decoded));
        EXPECT_EQ(decoded, data);
    }
}

TEST(Base64, RejectsInvalidCharacters) {
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(base64Decode("Zm9v!g==", false, out));  // '!' not in the alphabet
    EXPECT_FALSE(base64Decode("+++", true, out));        // '+' not in the URL-safe alphabet
}

TEST(Base64, RejectsBadPadding) {
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(base64Decode("Z===", false, out));   // padding before a real char is invalid
    EXPECT_FALSE(base64Decode("Zm9=v", false, out));  // '=' not trailing
}

TEST(Base64, RejectsLengthOneMod4) {
    std::vector<std::uint8_t> out;
    EXPECT_FALSE(base64Decode("Z", false, out));
    EXPECT_FALSE(base64Decode("Zm9vY", false, out));
}
