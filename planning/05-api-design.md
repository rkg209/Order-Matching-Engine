# Velox Matching Engine — API Design

**Version:** 1.0 | **Status:** Implementation-Ready | **Codename:** `velox`

---

## Table of Contents

1. [Overview](#1-overview)
2. [API Surface Map](#2-api-surface-map)
3. [Authentication Scheme](#3-authentication-scheme)
4. [Error Handling Conventions](#4-error-handling-conventions)
5. [Pagination Strategy](#5-pagination-strategy)
6. [Versioning Policy](#6-versioning-policy)
7. [OpenAPI Specification](#7-openapi-specification)
8. [Design Notes](#8-design-notes)

---

## 1. Overview

Velox exposes three distinct API surfaces, each serving a different client class with different latency and protocol requirements. These surfaces are not interchangeable — they reflect the system's strict separation between the hot path and everything else.

| Surface | Protocol | Port | Client Class | Latency Class |
|---|---|---|---|---|
| **Order Gateway** | Binary TCP (length-prefixed frames) | 9001 | Trading clients (automated) | Microseconds |
| **WebSocket Stream** | WebSocket / JSON | 8080 | Visualizer, market-data consumers | Milliseconds |
| **REST Management API** | HTTP/1.1 + JSON | 8081 | Administrative tooling, ops | Seconds |

The binary TCP protocol and WebSocket stream are specified in the architecture and system design documents. This document focuses on the **REST Management API** — the only surface that exposes a conventional request/response HTTP interface — while also documenting the authentication, error handling, pagination, and versioning conventions that apply uniformly across all three surfaces where applicable.

The REST Management API is served by a dedicated Boost.Beast HTTP server on port 8081. It is **never** on the hot path. It reads from and writes to the Tier 3 PostgreSQL database and the in-memory configuration caches loaded at startup. It does not interact with the matching engine thread, the SPSC ring buffers, or the journal.

---

## 2. API Surface Map

### 2.1 Binary TCP Order Gateway (Port 9001)

The order gateway is a stateful, session-oriented binary protocol. It is not REST. It is documented here for completeness and to establish the authentication model that the REST API mirrors.

**Session lifecycle:**

```
Client                          Gateway
  │                                │
  │──── TCP connect ──────────────▶│
  │                                │
  │──── LOGIN frame ──────────────▶│  { participantId, token, clientSeqNum=0 }
  │◀─── LOGIN_ACK or LOGIN_REJECT ─│
  │                                │
  │──── NEW_ORDER frame ──────────▶│  { clientSeqNum=1, orderId, ... }
  │◀─── EXEC_REPORT (NEW_ACK) ─────│
  │                                │
  │──── CANCEL frame ─────────────▶│  { clientSeqNum=2, orderId }
  │◀─── EXEC_REPORT (CANCELLED) ───│
  │                                │
  │──── HEARTBEAT ────────────────▶│  (every 30s if no other message)
  │◀─── HEARTBEAT ─────────────────│
  │                                │
  │──── TCP close ────────────────▶│
```

**Message types and wire sizes:**

| Message Type | Direction | Payload Bytes | Description |
|---|---|---|---|
| `LOGIN` | Client → Gateway | 41 | participantId (8) + token (32) + clientSeqNum (1) |
| `LOGIN_ACK` | Gateway → Client | 9 | sessionId (8) + status (1) |
| `LOGIN_REJECT` | Gateway → Client | 2 | rejectCode (1) + reason (1) |
| `NEW_ORDER` | Client → Gateway | 39 | See §1.5 of system design |
| `CANCEL` | Client → Gateway | 20 | See §1.5 of system design |
| `CANCEL_REPLACE` | Client → Gateway | 36 | See §1.5 of system design |
| `EXEC_REPORT` | Gateway → Client | 49 | See §1.5 of system design |
| `REJECT` | Gateway → Client | 17 | See §1.5 of system design |
| `HEARTBEAT` | Both | 8 | timestamp (8) |

### 2.2 WebSocket Stream (Port 8080, Path `/stream`)

Read-only. The server broadcasts JSON messages to all connected subscribers. Clients send no messages; any inbound message from a client is silently discarded.

**Message envelope:**

```json
{
  "type": "<MESSAGE_TYPE>",
  "seq":  12345,
  "ts":   1718000000000
}
```

**Message types:**

| Type | Direction | Description |
|---|---|---|
| `L2_UPDATE` | Server → Client | Incremental L2 book update for one price level |
| `TRADE` | Server → Client | A matched execution |
| `LATENCY` | Server → Client | 1 Hz histogram snapshot from `LatencyPublisher` |
| `INSTRUMENT_STATUS` | Server → Client | Trading state change for an instrument |

**`L2_UPDATE` payload:**

```json
{
  "type":         "L2_UPDATE",
  "seq":          12345,
  "ts":           1718000000000,
  "instrumentId": 1,
  "side":         "BID",
  "price":        1500000,
  "qty":          2500,
  "action":       "UPDATE"
}
```

`action` is one of `INSERT`, `UPDATE`, `DELETE`. `price` and `qty` are encoded as integers with four implied decimal places (× 10,000), matching the wire protocol and database schema exactly.

**`TRADE` payload:**

```json
{
  "type":           "TRADE",
  "seq":            12346,
  "ts":             1718000000001,
  "instrumentId":   1,
  "tradeId":        9001,
  "price":          1500000,
  "qty":            100,
  "aggressorSide":  "BUY"
}
```

**`LATENCY` payload:**

```json
{
  "type":      "LATENCY",
  "ts":        1718000000000,
  "p50Ns":     1200,
  "p99Ns":     4800,
  "p999Ns":    18000,
  "maxNs":     95000,
  "count":     48291,
  "ordersReceived":  48291,
  "ordersMatched":   31004,
  "ordersCancelled": 12100,
  "ordersRejected":  187,
  "tradesExecuted":  15502
}
```

### 2.3 REST Management API (Port 8081)

Conventional HTTP/1.1 + JSON. Covers participant management, instrument configuration, risk limits, session monitoring, and audit log access. Full specification in §7.

**Base URL:** `http://{host}:8081/api/v1`

**Resource groups:**

| Group | Path Prefix | Description |
|---|---|---|
| Participants | `/participants` | CRUD for participant accounts |
| Credentials | `/participants/{id}/credentials` | Credential lifecycle management |
| Instruments | `/instruments` | Instrument configuration |
| Trading Sessions | `/instruments/{id}/sessions` | Session scheduling |
| Risk Limits | `/participants/{id}/risk-limits` | Pre-trade risk controls |
| Orders | `/orders` | Audit-record query (read-only) |
| Trades | `/trades` | Trade history query (read-only) |
| Execution Reports | `/execution-reports` | Execution report query (read-only) |
| Journal Segments | `/journal/segments` | Journal metadata (read-only) |
| Snapshots | `/snapshots` | Snapshot metadata (read-only) |
| Audit Events | `/audit-events` | Compliance log query (read-only) |
| Engine Status | `/engine/status` | Live engine health and counters |

---

## 3. Authentication Scheme

### 3.1 Design Principles

Authentication in Velox is designed around one constraint that flows from the architecture: **the matching engine thread must never perform an authentication check**. Authentication is a gateway concern. By the time a command reaches the SPSC ring, it has already been authenticated and the `participantId` has been stamped onto the `CommandEvent`.

This produces two separate authentication mechanisms — one for the binary TCP gateway, one for the REST API — that share the same underlying credential store (`velox.participant_credential`) but differ in their token presentation and validation paths.

### 3.2 Binary TCP Gateway Authentication

**Mechanism:** Pre-shared token, presented in the `LOGIN` frame.

**Flow:**

1. The participant is issued a raw token (a 32-byte cryptographically random value) out-of-band by an administrator using the REST API (`POST /participants/{id}/credentials`).
2. The raw token is stored by the participant and never transmitted again after the initial issuance response.
3. The database stores `SHA-256(salt || raw_token)` and the 16-byte random `salt` in `velox.participant_credential`. The raw token is never persisted.
4. At gateway startup, `AuthHandler` loads all `ACTIVE` credentials into an in-memory `std::unordered_map<int64_t, CredentialRecord>` keyed by `participantId`. Each `CredentialRecord` holds the `token_hash` (`std::array<uint8_t,32>`) and `token_salt` (`std::array<uint8_t,16>`).
5. When a `LOGIN` frame arrives, `AuthHandler.check()` computes `SHA-256(stored_salt || presented_token)` and compares it to the stored hash using a constant-time comparison (OpenSSL `CRYPTO_memcmp`). This prevents timing attacks.
6. On success, the `ClientSession` is assigned the authenticated `participantId` and a `sessionId`. A `LOGIN_ACK` is sent.
7. On failure, a `LOGIN_REJECT` is sent with a `rejectCode`. The connection is closed after three consecutive failures from the same IP address (configurable via `VELOX_AUTH_MAX_FAILURES`).

**Credential cache invalidation:** When a credential is revoked via the REST API, the `AuthHandler` cache is updated in-place under a `std::shared_mutex` writer lock (a plain lock is fine here — this is off the hot path). Active sessions authenticated with the revoked credential are not immediately terminated — they complete their current session. New `LOGIN` attempts with the revoked token are rejected immediately.

**Token rotation:** A participant may have multiple `ACTIVE` credentials simultaneously (see `velox.participant_credential`). This enables zero-downtime key rotation: issue a new credential, update the client, then revoke the old credential.

### 3.3 REST Management API Authentication

**Mechanism:** HTTP Bearer token (JWT), validated against a separate administrative credential store.

The REST API is an administrative surface, not a trading surface. It uses a different authentication mechanism from the binary gateway to reflect the different trust model: REST API callers are human operators and automated ops tooling, not latency-sensitive trading systems.

**Token format:** JSON Web Token (JWT), signed with HMAC-SHA256 (`HS256`), issued/verified using `jwt-cpp`. The signing secret is configured via the `VELOX_ADMIN_JWT_SECRET` environment variable (minimum 32 bytes, base64-encoded). The JWT is stateless — no server-side session store is required.

**JWT claims:**

```json
{
  "sub":   "admin-user-id",
  "roles": ["ADMIN", "READ_ONLY"],
  "iat":   1718000000,
  "exp":   1718003600,
  "jti":   "unique-token-id"
}
```

**Required claims:**

| Claim | Type | Description |
|---|---|---|
| `sub` | string | Administrative user identifier |
| `roles` | string[] | Authorization roles (see §3.4) |
| `iat` | integer | Issued-at timestamp (Unix seconds) |
| `exp` | integer | Expiry timestamp (Unix seconds); maximum 1 hour from `iat` |
| `jti` | string | Unique token ID; used for revocation |

**Token presentation:** The client includes the JWT in the `Authorization` header:

```
Authorization: Bearer <jwt>
```

**Validation steps (performed by the REST API filter on every request):**

1. Extract the `Authorization` header. If absent or malformed, return `401 Unauthorized`.
2. Verify the JWT signature using the configured `VELOX_ADMIN_JWT_SECRET`.
3. Verify `exp` has not passed. If expired, return `401 Unauthorized` with `"code": "TOKEN_EXPIRED"`.
4. Verify `iat` is not in the future (clock skew tolerance: 30 seconds).
5. Check the `jti` against the revocation set (an in-memory `std::unordered_map<std::string, int64_t>` of jti → expiry, guarded by a `std::shared_mutex`). If present, return `401 Unauthorized` with `"code": "TOKEN_REVOKED"`.
6. Extract `roles` and attach to the request context for authorization checks.

**Token issuance:** The REST API does not issue tokens. Tokens are issued by an external identity provider or by the `scripts/issue-admin-token.sh` utility script (for development and CI). This keeps the REST API stateless and avoids the complexity of a token issuance endpoint.

**Token revocation:** `DELETE /auth/tokens/{jti}` adds the `jti` to the in-memory revocation set with a TTL equal to the token's remaining lifetime. The revocation set is not persisted — it is rebuilt from the `velox.audit_event` table on restart (events of type `TOKEN_REVOKED` are replayed into the set).

### 3.4 Authorization Roles

| Role | Scope | Permitted Operations |
|---|---|---|
| `ADMIN` | Full | All read and write operations |
| `RISK_MANAGER` | Risk | Read all; write `risk_limit`; read `order_audit`, `trade`, `execution_report` |
| `OPERATIONS` | Ops | Read all; write `trading_session` status; read journal and snapshot metadata |
| `READ_ONLY` | Read | GET on all resources; no POST, PUT, PATCH, DELETE |
| `AUDIT` | Compliance | Read `audit_event`, `order_audit`, `trade`, `execution_report` only |

Role checks are enforced at the handler level, not the filter level. Each endpoint declares its required role in the handler annotation. A request with insufficient role receives `403 Forbidden`.

### 3.5 Transport Security

**Development:** Plain HTTP on port 8081. TLS termination is the responsibility of a reverse proxy (nginx, Envoy) in production deployments.

**Production recommendation:** All REST API traffic should be TLS-terminated at a reverse proxy. The `VELOX_ADMIN_ALLOWED_CIDR` environment variable restricts the IP ranges from which the REST API accepts connections (default: `127.0.0.1/8, ::1/128` — localhost only). This is a defence-in-depth measure; TLS is still required.

**Binary TCP gateway:** TLS is not implemented in the initial build. The gateway is intended for use within a trusted network segment. TLS wrapping via stunnel or a sidecar proxy is the recommended approach for production.

---

## 4. Error Handling Conventions

### 4.1 Binary TCP Gateway Errors

The binary protocol uses the `REJECT` message (17 bytes) and `EXEC_REPORT` with `execType=REJECTED` for all error conditions. There is no concept of an HTTP status code.

**`REJECT` message fields:**

| Field | Type | Description |
|---|---|---|
| `orderId` | long (8) | The order ID from the rejected request |
| `rejectReason` | byte (1) | Numeric reason code (see below) |
| `globalSeq` | long (8) | Sequencer sequence number at rejection |

**Reject reason codes:**

| Code | Name | Description |
|---|---|---|
| `0x01` | `UNKNOWN_ORDER` | Cancel or replace references an order ID not in the book |
| `0x02` | `DUPLICATE_ORDER_ID` | `orderId` already exists in the book |
| `0x03` | `INVALID_PRICE` | Price ≤ 0, or not a multiple of tick size |
| `0x04` | `INVALID_QUANTITY` | Quantity ≤ 0, or not a multiple of lot size |
| `0x05` | `INVALID_SIDE` | Side byte is not `0x01` (BUY) or `0x02` (SELL) |
| `0x06` | `INVALID_ORDER_TYPE` | Order type byte is not a recognized value |
| `0x07` | `INSTRUMENT_NOT_FOUND` | `instrumentId` does not exist |
| `0x08` | `INSTRUMENT_NOT_OPEN` | Instrument is not in `OPEN` trading state |
| `0x09` | `RISK_LIMIT_BREACHED` | Order violates a pre-trade risk limit |
| `0x0A` | `SEQUENCE_GAP` | Client sequence number has a gap |
| `0x0B` | `DUPLICATE_CLIENT_SEQ` | Client sequence number already seen |
| `0x0C` | `SESSION_NOT_AUTHENTICATED` | Message received before successful `LOGIN` |
| `0x0D` | `SELF_TRADE_PREVENTED` | Order cancelled by self-trade prevention policy |
| `0x0E` | `PRICE_BAND_VIOLATION` | Price outside the instrument's price band |
| `0xFF` | `INTERNAL_ERROR` | Unexpected engine error; contact support |

**`LOGIN_REJECT` reason codes:**

| Code | Name | Description |
|---|---|---|
| `0x01` | `INVALID_CREDENTIALS` | Token does not match stored hash |
| `0x02` | `PARTICIPANT_SUSPENDED` | Participant status is `SUSPENDED` or `TERMINATED` |
| `0x03` | `CREDENTIAL_EXPIRED` | Credential `valid_until` has passed |
| `0x04` | `CREDENTIAL_REVOKED` | Credential status is `REVOKED` |
| `0x05` | `TOO_MANY_FAILURES` | IP address has exceeded `VELOX_AUTH_MAX_FAILURES` |
| `0x06` | `ALREADY_LOGGED_IN` | This `participantId` already has an active session |

### 4.2 REST API Error Conventions

All REST API errors follow a single envelope structure. This structure is returned for every non-2xx response, without exception. Clients must not attempt to parse error details from any other field.

**Error envelope:**

```json
{
  "error": {
    "code":      "PARTICIPANT_NOT_FOUND",
    "message":   "No participant exists with id 9999.",
    "requestId": "req_01J2K3M4N5P6Q7R8S9T0",
    "timestamp": "2024-06-10T12:34:56.789Z",
    "details":   []
  }
}
```

**Envelope fields:**

| Field | Type | Always Present | Description |
|---|---|---|---|
| `error.code` | string | Yes | Machine-readable error code. Stable across versions. Suitable for programmatic handling. |
| `error.message` | string | Yes | Human-readable description. May change between versions; do not parse. |
| `error.requestId` | string | Yes | Unique identifier for this request, for log correlation. Included in server logs. |
| `error.timestamp` | string (ISO 8601) | Yes | Server time at which the error was generated. |
| `error.details` | array | Yes (may be empty) | Structured detail objects for validation errors (see §4.2.1). |

**HTTP status code mapping:**

| HTTP Status | When Used | Example `error.code` Values |
|---|---|---|
| `400 Bad Request` | Request body or query parameter validation failure | `VALIDATION_ERROR`, `INVALID_PRICE_FORMAT`, `MISSING_REQUIRED_FIELD` |
| `401 Unauthorized` | Missing, invalid, or expired authentication token | `MISSING_TOKEN`, `INVALID_TOKEN`, `TOKEN_EXPIRED`, `TOKEN_REVOKED` |
| `403 Forbidden` | Authenticated but insufficient role | `INSUFFICIENT_ROLE` |
| `404 Not Found` | Resource does not exist | `PARTICIPANT_NOT_FOUND`, `INSTRUMENT_NOT_FOUND`, `ORDER_NOT_FOUND` |
| `409 Conflict` | State conflict (duplicate, wrong status) | `PARTICIPANT_CODE_ALREADY_EXISTS`, `CREDENTIAL_ALREADY_REVOKED`, `INSTRUMENT_ALREADY_OPEN` |
| `422 Unprocessable Entity` | Semantically invalid request (passes schema validation but violates business rules) | `TICK_SIZE_VIOLATION`, `RISK_LIMIT_CONFLICT`, `SESSION_OVERLAP` |
| `429 Too Many Requests` | Rate limit exceeded | `RATE_LIMIT_EXCEEDED` |
| `500 Internal Server Error` | Unexpected server error | `INTERNAL_ERROR` |
| `503 Service Unavailable` | Engine not ready (e.g., recovery in progress) | `ENGINE_NOT_READY` |

**Rule:** `error.code` values are SCREAMING_SNAKE_CASE strings. They are stable identifiers — a given code will always mean the same thing and will not be removed within a major API version. New codes may be added in minor versions.

#### 4.2.1 Validation Error Details

When the HTTP status is `400` and the error is a field-level validation failure, the `details` array contains one object per failing field:

```json
{
  "error": {
    "code":      "VALIDATION_ERROR",
    "message":   "Request body contains 2 validation error(s).",
    "requestId": "req_01J2K3M4N5P6Q7R8S9T0",
    "timestamp": "2024-06-10T12:34:56.789Z",
    "details": [
      {
        "field":   "tickSize",
        "code":    "MUST_BE_POSITIVE",
        "message": "tickSize must be greater than 0."
      },
      {
        "field":   "symbol",
        "code":    "MAX_LENGTH_EXCEEDED",
        "message": "symbol must not exceed 16 characters."
      }
    ]
  }
}
```

**Detail object fields:**

| Field | Type | Description |
|---|---|---|
| `field` | string | JSON path to the failing field (dot notation for nested fields, e.g., `address.postCode`) |
| `code` | string | Machine-readable validation code |
| `message` | string | Human-readable description |

**Validation error codes:**

| Code | Description |
|---|---|
| `REQUIRED` | Field is required but absent or null |
| `MUST_BE_POSITIVE` | Numeric field must be > 0 |
| `MUST_BE_NON_NEGATIVE` | Numeric field must be ≥ 0 |
| `MAX_LENGTH_EXCEEDED` | String field exceeds maximum length |
| `INVALID_ENUM_VALUE` | Value is not a member of the allowed set |
| `INVALID_FORMAT` | Value does not match the expected format (e.g., ISO 8601 date) |
| `INVALID_RANGE` | Numeric value is outside the allowed range |
| `PATTERN_MISMATCH` | String value does not match the required pattern |

### 4.3 WebSocket Stream Errors

The WebSocket stream is read-only and does not accept client messages, so there are no client-originated errors to report. Server-side errors that affect the stream are communicated as a special JSON message before the connection is closed:

```json
{
  "type":    "ERROR",
  "code":    "STREAM_OVERRUN",
  "message": "Subscriber fell too far behind; connection closed.",
  "ts":      1718000000000
}
```

**Stream error codes:**

| Code | Description |
|---|---|
| `STREAM_OVERRUN` | The subscriber's send buffer is full; the server is closing the connection to protect the broadcast path. |
| `SERVER_SHUTDOWN` | The server is shutting down gracefully. |
| `INTERNAL_ERROR` | Unexpected server error. |

Clients should implement exponential backoff reconnection with jitter. The recommended initial backoff is 100 ms, maximum 30 s, multiplier 2.0, jitter ±20%.

### 4.4 Request Tracing

Every REST API request is assigned a `requestId` of the form `req_{26-char ULID}`. This ID is:

- Included in the response body in the `error.requestId` field (on errors) and in the `X-Request-Id` response header (on all responses).
- Written to the server access log alongside the HTTP method, path, status code, and latency.
- Written to the `velox.audit_event` table for `WARN`, `ERROR`, and `CRITICAL` severity events.

Clients should log the `X-Request-Id` header value when reporting issues.

---

## 5. Pagination Strategy

### 5.1 Applicability

Pagination applies to all REST API endpoints that return collections. The following endpoints return collections and are paginated:

- `GET /participants`
- `GET /participants/{id}/credentials`
- `GET /instruments`
- `GET /instruments/{id}/sessions`
- `GET /participants/{id}/risk-limits`
- `GET /orders`
- `GET /trades`
- `GET /execution-reports`
- `GET /journal/segments`
- `GET /snapshots`
- `GET /audit-events`

Endpoints that return a single resource (`GET /participants/{id}`, etc.) are not paginated.

### 5.2 Pagination Mechanism: Cursor-Based

Velox uses **cursor-based pagination** (also called keyset pagination) rather than offset-based pagination. This choice is driven by the nature of the data:

- `order_audit`, `trade`, `execution_report`, and `audit_event` are append-only tables with monotonically increasing surrogate keys. Offset pagination on these tables produces inconsistent results when new rows are inserted between pages.
- The `global_seq` column on `order_audit`, `trade`, and `execution_report` provides a stable, monotonically increasing cursor that maps directly to the sequencer's ordering guarantee.
- Cursor-based pagination has O(1) seek cost via the primary key index, regardless of page depth. Offset pagination degrades to O(n) for deep pages.

**Offset pagination is not supported.** The `offset` query parameter is not accepted and will produce a `400 Bad Request` with `"code": "UNSUPPORTED_PARAMETER"`.

### 5.3 Cursor Format

The cursor is an opaque, base64url-encoded string. Clients must treat it as opaque — its internal structure is not part of the API contract and may change between minor versions.

Internally, the cursor encodes the primary key value (or `global_seq` for time-ordered resources) of the last item on the previous page, plus a sort direction indicator. The server decodes the cursor, validates it, and uses it in a `WHERE id > :cursor` (or `WHERE id < :cursor` for reverse pagination) clause.

**Cursor validity:** Cursors are valid indefinitely for append-only tables (`order_audit`, `trade`, `execution_report`, `audit_event`). For mutable tables (`participant`, `instrument`, etc.), cursors are valid for 24 hours. An expired or invalid cursor returns `400 Bad Request` with `"code": "INVALID_CURSOR"`.

### 5.4 Request Parameters

All paginated endpoints accept the following query parameters:

| Parameter | Type | Default | Description |
|---|---|---|---|
| `limit` | integer | `50` | Number of items per page. Minimum: `1`. Maximum: `1000`. |
| `cursor` | string | absent | Opaque cursor from the previous page's `pagination.nextCursor`. Absent on the first request. |
| `sort` | string | resource-specific | Sort order. Accepted values are resource-specific (see §5.6). |

### 5.5 Response Envelope

All paginated responses wrap the item array in a standard envelope:

```json
{
  "data": [ ... ],
  "pagination": {
    "limit":      50,
    "count":      50,
    "nextCursor": "eyJpZCI6MTIzNH0",
    "hasMore":    true
  }
}
```

**Pagination envelope fields:**

| Field | Type | Description |
|---|---|---|
| `data` | array | The items for this page. |
| `pagination.limit` | integer | The `limit` value used for this page (echoed from the request). |
| `pagination.count` | integer | The number of items in `data` for this page. Always ≤ `limit`. |
| `pagination.nextCursor` | string \| null | Cursor to pass as `cursor` on the next request. `null` when `hasMore` is `false`. |
| `pagination.hasMore` | boolean | `true` if there are more items beyond this page. |

**Last page:** When `hasMore` is `false`, `nextCursor` is `null`. Clients must check `hasMore` rather than checking whether `count < limit`, because the last page may coincidentally contain exactly `limit` items.

**Empty result:** When no items match the query, `data` is `[]`, `count` is `0`, `hasMore` is `false`, and `nextCursor` is `null`.

### 5.6 Sort Orders

Each resource supports a defined set of sort orders. The `sort` parameter accepts a field name optionally prefixed with `-` for descending order.

| Resource | Supported `sort` Values | Default |
|---|---|---|
| `participants` | `participantCode`, `-participantCode`, `createdAt`, `-createdAt` | `participantCode` |
| `instruments` | `symbol`, `-symbol`, `createdAt`, `-createdAt` | `symbol` |
| `orders` | `globalSeq`, `-globalSeq`, `submittedAt`, `-submittedAt` | `-globalSeq` |
| `trades` | `globalSeq`, `-globalSeq`, `tradedAt`, `-tradedAt` | `-globalSeq` |
| `execution-reports` | `globalSeq`, `-globalSeq`, `reportedAt`, `-reportedAt` | `-globalSeq` |
| `audit-events` | `auditEventId`, `-auditEventId`, `occurredAt`, `-occurredAt` | `-auditEventId` |
| `journal/segments` | `segmentIndex`, `-segmentIndex` | `segmentIndex` |
| `snapshots` | `globalSeq`, `-globalSeq`, `createdAt`, `-createdAt` | `-globalSeq` |

An unsupported `sort` value returns `400 Bad Request` with `"code": "INVALID_SORT_FIELD"`.

### 5.7 Filtering

Paginated endpoints accept resource-specific filter parameters as query parameters. Filters are applied before pagination. All filter parameters are optional.

**Common filter parameters (where applicable):**

| Parameter | Type | Applicable To | Description |
|---|---|---|---|
| `participantId` | integer | orders, trades, execution-reports, audit-events | Filter by participant |
| `instrumentId` | integer | orders, trades, execution-reports, sessions | Filter by instrument |
| `fromSeq` | integer | orders, trades, execution-reports | Filter by `global_seq ≥ fromSeq` |
| `toSeq` | integer | orders, trades, execution-reports | Filter by `global_seq ≤ toSeq` |
| `fromTime` | ISO 8601 | orders, trades, audit-events | Filter by timestamp ≥ fromTime |
| `toTime` | ISO 8601 | orders, trades, audit-events | Filter by timestamp ≤ toTime |
| `status` | string | participants, instruments, credentials | Filter by status field |
| `side` | string (`BUY`\|`SELL`) | orders, trades | Filter by side |
| `execType` | string | execution-reports | Filter by `exec_type` |
| `eventType` | string | audit-events | Filter by `event_type`