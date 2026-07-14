# Spec 013 — REST admin API

**Status:** ⛔ DEFERRED — optional · **Blocks:** nothing · **Nothing may depend on this**

> ⚠️ Preserved from `planning/05-api-design.md` + `05-openapi.yaml`. **Not approved.** It largely depends
> on Spec 012 (the Postgres tier), which is itself deferred as contradicting CON-8.

## What it would be

A JWT-authenticated HTTP/JSON management surface (`/api/v1`) for administering participants,
credentials, instruments, trading sessions, and risk limits — plus operational reads (engine status,
journal segments, snapshots, audit events).

This is the **third** API surface, and the slowest by three orders of magnitude:

| Surface | Protocol | Port | Latency class |
|---|---|---|---|
| Order gateway | Binary TCP, length-prefixed | 9001 | **microseconds** |
| Market-data / visualizer | WebSocket, JSON | 8080 | milliseconds |
| REST admin | HTTP/1.1, JSON | 8081 ⚠️ | seconds |

Auth would be **JWT HS256** via `jwt-cpp`, `Authorization: Bearer`, secret in `VELOX_ADMIN_JWT_SECRET`,
claims `sub`/`roles`/`iat`/`exp`(≤1 h)/`jti`, with an in-memory `jti` revocation set.

## Why it is deferred

1. **Most of it manages entities that only exist in the deferred Postgres tier** — participants,
   credentials, risk limits, trading sessions. With Spec 012 deferred, there is nothing behind most of
   these endpoints. The dependency is not incidental; it is most of the surface area.
2. **It is not on the critical path to anything the project is selling.** Nothing about the latency
   story, the correctness story, or the recovery story needs a REST API. It is administrative
   convenience, and every hour spent on it is an hour not spent on the engine.
3. **The source documents contradict each other badly enough that this could not be built from them
   as-is** (below).

## The source docs are broken — do not transcribe them

1. **`05-openapi.yaml` does not parse.** It is truncated mid-response inside
   `DELETE /participants/{id}/risk-limits/{riskLimitId}`, and its **entire `components:` block is
   missing** — while ~50 `$ref: '#/components/...'` pointers reference into it. It also begins with a
   literal ```` ```yaml ```` markdown fence. It is not a loadable OpenAPI document.
2. **Port conflict.** `05-api-design.md` puts REST on **8081** (`http://{host}:8081/api/v1`). The
   `servers:` block in `05-openapi.yaml` says **`http://localhost:8080/v1`** — which **collides with the
   WebSocket/visualizer port** and drops the `/api` prefix.
3. **Token issuance contradiction.** `05-api-design.md` §3.3 says, explicitly, *"The REST API does not
   issue tokens"* — they come from `scripts/issue-admin-token.sh`, deliberately, to avoid the complexity
   of an issuance endpoint. `05-openapi.yaml` then **defines `POST /auth/token`** and states in its
   `info` block that all endpoints require a token *"issued by the `/v1/auth/token` endpoint."* Revocation
   contradicts too: `DELETE /auth/tokens/{jti}` (md) vs `POST /auth/token/revoke` (yaml).
4. **Pagination contract contradiction.** The `.md` uses `limit`/`cursor`/`sort` with a camelCase envelope
   (`nextCursor`, `hasMore`); the `.yaml` uses `after`/`limit` with a snake_case envelope (`next_cursor`,
   `has_more`).
5. **Authorization model contradiction.** The `.md` uses **roles** (`ADMIN`, `RISK_MANAGER`,
   `OPERATIONS`, `READ_ONLY`, `AUDIT`); the `.yaml` uses **scopes** (`admin`, `read`).
6. **Half the promised endpoints were never written** — `/orders`, `/trades`, `/execution-reports`,
   `/journal/segments`, `/snapshots`, `/audit-events`, `/engine/status` are all promised in §2.3 of the
   `.md` and absent from the `.yaml`.

Six contradictions in two documents that were meant to specify one API. Any implementation would be
picking arbitrarily between them — which means the design decision has not actually been made yet, and
pretending otherwise by writing code would just hide that.

## If revived

- Redesign the API contract from scratch. **Do not** try to reconcile the two documents; decide fresh.
- **`/engine/status` and `/snapshots` are the only endpoints that do not need Postgres** — they read the
  engine and the filesystem. If a *small* admin surface is ever wanted, that is the whole of it, and it
  could ship without Spec 012 at all.
- Whatever is built: **the engine must not know it exists.** The admin API observes; it does not reach
  into the hot path, and no engine code links against it.
