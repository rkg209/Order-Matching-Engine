#pragma once

// Spec 013 T3: JWT HS256 verification (RFC 7519 + RFC 7515 §HMAC). Verify-only -- velox_adminctl
// never issues or signs a token; that is scripts/issue-admin-token.sh's job, run offline. Claims
// required by admin/API.md: sub, roles (array of strings), iat, exp, jti. `alg` must be exactly
// "HS256" -- "none" and any other algorithm are rejected, never silently accepted.
//
// This is a minimal, purpose-built JSON claim reader, not a general JSON parser: the header and
// payload objects this verifies are always flat (no nesting beyond the `roles` array), because
// this project mints every token itself via scripts/issue-admin-token.sh.

#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

namespace velox::admin {

enum class JwtReason {
    Ok,
    Malformed,    // not exactly header.payload.signature, or a segment fails base64url decode
    AlgRejected,  // alg missing, "none", or anything other than exactly "HS256"
    BadSignature,
    MissingClaim,  // sub / roles / iat / exp / jti missing from the payload
    TtlTooLong,    // exp - iat > 3600
    NotYetValid,   // iat is in the future
    Expired,       // now > exp
    Revoked,       // jti present in the caller-supplied denylist
};

struct VerifyResult {
    bool ok = false;
    std::string sub;
    std::vector<std::string> roles;
    JwtReason reason = JwtReason::Ok;
};

VerifyResult verifyJwt(const std::string& token, const std::string& secret, std::int64_t nowUnix,
                       const std::unordered_set<std::string>& revokedJti);

}  // namespace velox::admin
