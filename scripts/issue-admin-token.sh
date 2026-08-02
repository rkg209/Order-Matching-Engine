#!/usr/bin/env bash
# Spec 013 T6: mints an HS256 JWT for velox_adminctl from VELOX_ADMIN_JWT_SECRET, offline. There
# is deliberately no /auth/* HTTP route (plan decision: "the API never issues tokens") -- this
# script is the only issuer, run by an operator, never by the daemon itself. Uses only openssl +
# coreutils, no jwt-cpp, matching admin/hmac_sha256.hpp's zero-extra-dependency posture.
#
#   VELOX_ADMIN_JWT_SECRET=... ./scripts/issue-admin-token.sh --sub=rahul --roles=ADMIN --ttl=600

set -euo pipefail

SUB=""
ROLES=""
TTL=600
JTI=""

for arg in "$@"; do
    case "$arg" in
        --sub=*) SUB="${arg#--sub=}" ;;
        --roles=*) ROLES="${arg#--roles=}" ;;
        --ttl=*) TTL="${arg#--ttl=}" ;;
        --jti=*) JTI="${arg#--jti=}" ;;
        *)
            echo "unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

if [[ -z "$SUB" || -z "$ROLES" ]]; then
    echo "usage: issue-admin-token.sh --sub=NAME --roles=ADMIN,OPERATIONS,READ_ONLY [--ttl=SECONDS] [--jti=ID]" >&2
    exit 2
fi

if [[ -z "${VELOX_ADMIN_JWT_SECRET:-}" ]]; then
    echo "VELOX_ADMIN_JWT_SECRET is not set" >&2
    exit 2
fi

if (( TTL > 3600 )); then
    echo "--ttl must be <= 3600 (admin/jwt.hpp rejects exp - iat > 3600)" >&2
    exit 2
fi

if [[ -z "$JTI" ]]; then
    JTI="$(openssl rand -hex 8)"
fi

IAT="$(date +%s)"
EXP=$((IAT + TTL))

# ROLES "ADMIN,OPERATIONS" -> ["ADMIN","OPERATIONS"]
ROLES_JSON="[$(echo "$ROLES" | awk -F',' '{ for (i=1;i<=NF;i++) { printf "%s\"%s\"", (i>1?",":""), $i } }')]"

HEADER='{"alg":"HS256","typ":"JWT"}'
PAYLOAD="{\"sub\":\"$SUB\",\"roles\":$ROLES_JSON,\"iat\":$IAT,\"exp\":$EXP,\"jti\":\"$JTI\"}"

b64url() {
    openssl base64 -A | tr '+/' '-_' | tr -d '='
}

HEADER_B64="$(printf '%s' "$HEADER" | b64url)"
PAYLOAD_B64="$(printf '%s' "$PAYLOAD" | b64url)"
SIGNING_INPUT="${HEADER_B64}.${PAYLOAD_B64}"

SIGNATURE_B64="$(printf '%s' "$SIGNING_INPUT" | \
    openssl dgst -sha256 -hmac "$VELOX_ADMIN_JWT_SECRET" -binary | b64url)"

echo "${SIGNING_INPUT}.${SIGNATURE_B64}"
