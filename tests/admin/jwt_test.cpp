// Spec 013 T3: admin::verifyJwt coverage per the revival plan's required case list -- valid;
// expired; iat in the future; alg:none; alg swapped to a different algorithm; tampered payload;
// tampered signature; missing exp; exp-iat too long; missing jti; revoked jti; malformed
// (2-segment and 4-segment) tokens.

#include <gtest/gtest.h>

#include <string>
#include <unordered_set>
#include <vector>

#include "admin/hmac_sha256.hpp"
#include "admin/jwt.hpp"
#include "common/base64.hpp"

using namespace velox::admin;

namespace {

const std::string kSecret = "test-secret-do-not-use-in-prod";

std::string b64url(const std::string& s) {
    return velox::common::base64Encode(s, /*urlSafe=*/true, /*pad=*/false);
}

// Builds a signed token from raw header/payload JSON strings, so tests can construct malformed
// or borderline payloads directly rather than only through a "happy path" builder.
std::string signToken(const std::string& headerJson, const std::string& payloadJson,
                      const std::string& secret) {
    const std::string signingInput = b64url(headerJson) + "." + b64url(payloadJson);
    const Sha256Digest sig = hmacSha256(secret, signingInput);
    return signingInput + "." +
           velox::common::base64Encode(sig.data(), sig.size(), /*urlSafe=*/true, /*pad=*/false);
}

std::string payloadJson(std::int64_t iat, std::int64_t exp, const std::string& sub = "rahul",
                        const std::string& jti = "jti-1",
                        const std::string& rolesJson = "[\"ADMIN\"]") {
    return "{\"sub\":\"" + sub + "\",\"roles\":" + rolesJson + ",\"iat\":" + std::to_string(iat) +
           ",\"exp\":" + std::to_string(exp) + ",\"jti\":\"" + jti + "\"}";
}

constexpr const char* kHs256Header = R"({"alg":"HS256","typ":"JWT"})";

}  // namespace

TEST(Jwt, ValidTokenVerifies) {
    const std::int64_t now = 1000;
    const std::string token = signToken(kHs256Header, payloadJson(now, now + 600), kSecret);
    const auto r = verifyJwt(token, kSecret, now + 100, {});
    ASSERT_TRUE(r.ok);
    EXPECT_EQ(r.sub, "rahul");
    ASSERT_EQ(r.roles.size(), 1u);
    EXPECT_EQ(r.roles[0], "ADMIN");
}

TEST(Jwt, Expired) {
    const std::int64_t now = 1000;
    const std::string token = signToken(kHs256Header, payloadJson(now, now + 100), kSecret);
    const auto r = verifyJwt(token, kSecret, now + 1000, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::Expired);
}

TEST(Jwt, IatInFuture) {
    const std::int64_t now = 1000;
    const std::string token = signToken(kHs256Header, payloadJson(now + 500, now + 900), kSecret);
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::NotYetValid);
}

TEST(Jwt, AlgNoneRejected) {
    const std::int64_t now = 1000;
    const std::string token =
        signToken(R"({"alg":"none","typ":"JWT"})", payloadJson(now, now + 600), kSecret);
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::AlgRejected);
}

TEST(Jwt, AlgSwappedToHs512Rejected) {
    const std::int64_t now = 1000;
    const std::string token =
        signToken(R"({"alg":"HS512","typ":"JWT"})", payloadJson(now, now + 600), kSecret);
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::AlgRejected);
}

TEST(Jwt, TamperedPayloadRejected) {
    const std::int64_t now = 1000;
    std::string token = signToken(kHs256Header, payloadJson(now, now + 600), kSecret);
    const std::size_t dot1 = token.find('.');
    const std::size_t dot2 = token.find('.', dot1 + 1);
    // Flip one character in the payload segment (still valid base64url, still decodes) -- the
    // signature no longer matches.
    std::string payloadSeg = token.substr(dot1 + 1, dot2 - dot1 - 1);
    payloadSeg[0] = (payloadSeg[0] == 'A') ? 'B' : 'A';
    token = token.substr(0, dot1 + 1) + payloadSeg + token.substr(dot2);

    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::BadSignature);
}

TEST(Jwt, TamperedSignatureRejected) {
    const std::int64_t now = 1000;
    std::string token = signToken(kHs256Header, payloadJson(now, now + 600), kSecret);
    token.back() = (token.back() == 'A') ? 'B' : 'A';
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::BadSignature);
}

TEST(Jwt, MissingExpRejected) {
    const std::int64_t now = 1000;
    const std::string payload =
        "{\"sub\":\"rahul\",\"roles\":[\"ADMIN\"],\"iat\":" + std::to_string(now) +
        ",\"jti\":\"jti-1\"}";
    const std::string token = signToken(kHs256Header, payload, kSecret);
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::MissingClaim);
}

TEST(Jwt, MissingJtiRejected) {
    const std::int64_t now = 1000;
    const std::string payload =
        "{\"sub\":\"rahul\",\"roles\":[\"ADMIN\"],\"iat\":" + std::to_string(now) +
        ",\"exp\":" + std::to_string(now + 600) + "}";
    const std::string token = signToken(kHs256Header, payload, kSecret);
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::MissingClaim);
}

TEST(Jwt, TtlTooLongRejected) {
    const std::int64_t now = 1000;
    const std::string token = signToken(kHs256Header, payloadJson(now, now + 3601), kSecret);
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::TtlTooLong);
}

TEST(Jwt, TtlExactlyOneHourAccepted) {
    const std::int64_t now = 1000;
    const std::string token = signToken(kHs256Header, payloadJson(now, now + 3600), kSecret);
    const auto r = verifyJwt(token, kSecret, now, {});
    EXPECT_TRUE(r.ok);
}

TEST(Jwt, RevokedJtiRejected) {
    const std::int64_t now = 1000;
    const std::string token =
        signToken(kHs256Header, payloadJson(now, now + 600, "rahul", "revoked-jti"), kSecret);
    const std::unordered_set<std::string> denylist = {"revoked-jti"};
    const auto r = verifyJwt(token, kSecret, now, denylist);
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::Revoked);
}

TEST(Jwt, TwoSegmentTokenRejected) {
    const auto r = verifyJwt("onlyoneheader.onlyonepayload", kSecret, 1000, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::Malformed);
}

TEST(Jwt, FourSegmentTokenRejected) {
    const std::int64_t now = 1000;
    const std::string token = signToken(kHs256Header, payloadJson(now, now + 600), kSecret);
    const auto r = verifyJwt(token + ".extra", kSecret, now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::Malformed);
}

TEST(Jwt, WrongSecretRejected) {
    const std::int64_t now = 1000;
    const std::string token = signToken(kHs256Header, payloadJson(now, now + 600), kSecret);
    const auto r = verifyJwt(token, "a-completely-different-secret", now, {});
    EXPECT_FALSE(r.ok);
    EXPECT_EQ(r.reason, JwtReason::BadSignature);
}
