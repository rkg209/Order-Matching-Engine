#include "admin/jwt.hpp"

#include <algorithm>
#include <cctype>

#include "admin/hmac_sha256.hpp"
#include "common/base64.hpp"

namespace velox::admin {

namespace {

std::vector<std::string> splitOn(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t pos = s.find(delim, start);
        if (pos == std::string::npos) {
            parts.push_back(s.substr(start));
            break;
        }
        parts.push_back(s.substr(start, pos - start));
        start = pos + 1;
    }
    return parts;
}

// Finds `"key"` followed by `:` and returns the position just after the colon (skipping
// whitespace), or std::string::npos if the key is not present as a top-level field.
std::size_t findValueStart(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    std::size_t pos = json.find(needle);
    if (pos == std::string::npos) return std::string::npos;
    pos += needle.size();
    pos = json.find_first_not_of(" \t\r\n", pos);
    if (pos == std::string::npos || json[pos] != ':') return std::string::npos;
    ++pos;
    pos = json.find_first_not_of(" \t\r\n", pos);
    return pos;
}

bool extractString(const std::string& json, const std::string& key, std::string& out) {
    const std::size_t start = findValueStart(json, key);
    if (start == std::string::npos || start >= json.size() || json[start] != '"') return false;
    const std::size_t end = json.find('"', start + 1);
    if (end == std::string::npos) return false;
    out = json.substr(start + 1, end - start - 1);
    return true;
}

bool extractInt(const std::string& json, const std::string& key, std::int64_t& out) {
    const std::size_t start = findValueStart(json, key);
    if (start == std::string::npos) return false;
    std::size_t end = start;
    while (end < json.size() &&
           (std::isdigit(static_cast<unsigned char>(json[end])) || json[end] == '-')) {
        ++end;
    }
    if (end == start) return false;
    out = std::stoll(json.substr(start, end - start));
    return true;
}

bool extractStringArray(const std::string& json, const std::string& key,
                        std::vector<std::string>& out) {
    const std::size_t start = findValueStart(json, key);
    if (start == std::string::npos || start >= json.size() || json[start] != '[') return false;
    const std::size_t end = json.find(']', start);
    if (end == std::string::npos) return false;
    out.clear();
    std::size_t i = start + 1;
    while (i < end) {
        const std::size_t q1 = json.find('"', i);
        if (q1 == std::string::npos || q1 >= end) break;
        const std::size_t q2 = json.find('"', q1 + 1);
        if (q2 == std::string::npos || q2 > end) break;
        out.push_back(json.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return true;
}

VerifyResult fail(JwtReason reason) {
    VerifyResult r;
    r.ok = false;
    r.reason = reason;
    return r;
}

}  // namespace

VerifyResult verifyJwt(const std::string& token, const std::string& secret, std::int64_t nowUnix,
                       const std::unordered_set<std::string>& revokedJti) {
    const std::vector<std::string> parts = splitOn(token, '.');
    if (parts.size() != 3) {
        return fail(JwtReason::Malformed);
    }
    const std::string& headerB64 = parts[0];
    const std::string& payloadB64 = parts[1];
    const std::string& sigB64 = parts[2];

    std::string headerJson;
    std::string payloadJson;
    std::vector<std::uint8_t> sigBytes;
    if (!common::base64Decode(headerB64, /*urlSafe=*/true, headerJson) ||
        !common::base64Decode(payloadB64, /*urlSafe=*/true, payloadJson) ||
        !common::base64Decode(sigB64, /*urlSafe=*/true, sigBytes)) {
        return fail(JwtReason::Malformed);
    }

    std::string alg;
    if (!extractString(headerJson, "alg", alg) || alg != "HS256") {
        return fail(JwtReason::AlgRejected);
    }

    if (sigBytes.size() != 32) {
        return fail(JwtReason::BadSignature);
    }
    Sha256Digest presented{};
    std::copy(sigBytes.begin(), sigBytes.end(), presented.begin());
    const std::string signingInput = headerB64 + "." + payloadB64;
    const Sha256Digest expected = hmacSha256(secret, signingInput);
    if (!constantTimeEquals(presented, expected)) {
        return fail(JwtReason::BadSignature);
    }

    std::string sub;
    std::string jti;
    std::vector<std::string> roles;
    std::int64_t iat = 0;
    std::int64_t exp = 0;
    if (!extractString(payloadJson, "sub", sub) || !extractString(payloadJson, "jti", jti) ||
        !extractStringArray(payloadJson, "roles", roles) || !extractInt(payloadJson, "iat", iat) ||
        !extractInt(payloadJson, "exp", exp)) {
        return fail(JwtReason::MissingClaim);
    }
    if (roles.empty()) {
        return fail(JwtReason::MissingClaim);
    }

    if (exp - iat > 3600) {
        return fail(JwtReason::TtlTooLong);
    }
    if (iat > nowUnix) {
        return fail(JwtReason::NotYetValid);
    }
    if (nowUnix > exp) {
        return fail(JwtReason::Expired);
    }
    if (revokedJti.find(jti) != revokedJti.end()) {
        return fail(JwtReason::Revoked);
    }

    VerifyResult r;
    r.ok = true;
    r.sub = sub;
    r.roles = roles;
    r.reason = JwtReason::Ok;
    return r;
}

}  // namespace velox::admin
