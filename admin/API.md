# velox_adminctl — REST admin API (Spec 013, revived at narrow read-only scope)

A standalone, read-only, JWT-authenticated HTTP/JSON daemon that observes a `velox_gateway` (or
`velox_live`) deployment's journal and snapshot directories **off disk**. It never links
`velox_gateway`, never touches a live `OrderBook`, and is invisible to `engine/`, `book/`, `ipc/`,
`runtime/`, `gateway/`, and `marketdata/` — the same isolation `.specify/memory/constitution.md`
v1.1's Tier-3 carve-out already required of `velox_auditd` (Spec 012), extended to this daemon by
v1.2.

Three API surfaces exist in this project, at three very different latency classes:

| Surface | Protocol | Port | Latency class |
|---|---|---|---|
| Order gateway | Binary TCP, length-prefixed | 9001 | microseconds |
| Market-data / visualizer | WebSocket, JSON | 8080 | milliseconds |
| REST admin | HTTP/1.1, JSON | **8081** | seconds |

## Why "source":"disk"

`/api/v1/engine/status` is a **durability-plane** status, derived from what is on disk, not live
engine counters. It cannot report `processedCount`, `fullSpins`, or `droppedRoutes` — those atomics
live only inside the `velox_gateway` process (`runtime/matching_thread.hpp`, `gateway/gateway.hpp`).
Every status response carries `"source":"disk"` so no consumer mistakes it for live telemetry.

## Running it

```bash
export VELOX_ADMIN_JWT_SECRET=$(openssl rand -hex 32)
./build/apps/velox_adminctl --journal=/path/to/journal-root --port=8081 [--bind=127.0.0.1] \
                            [--revoked=/path/to/revoked-jtis.txt]

TOKEN=$(VELOX_ADMIN_JWT_SECRET=$VELOX_ADMIN_JWT_SECRET \
        ./scripts/issue-admin-token.sh --sub=rahul --roles=ADMIN --ttl=600)

curl -s localhost:8081/healthz
curl -s -H "Authorization: Bearer $TOKEN" localhost:8081/api/v1/engine/status
curl -s -H "Authorization: Bearer $TOKEN" localhost:8081/api/v1/instruments
curl -s -H "Authorization: Bearer $TOKEN" localhost:8081/api/v1/instruments/1/journal/segments
curl -s -H "Authorization: Bearer $TOKEN" localhost:8081/api/v1/instruments/1/snapshots
```

`--journal=` points at the same root a sharded `velox_gateway --journal=ROOT` was given (layout
`<root>/shard-<id>/{journal,snapshots}`), or at a pre-Spec-011 flat single-instrument root
(`<root>/{journal,snapshots}`, reported as instrument id `1`).

`SIGHUP` reloads `--revoked=FILE` (one `jti` per line) without a restart.

## Auth

JWT **HS256** only. `Authorization: Bearer <token>`. Secret from **`VELOX_ADMIN_JWT_SECRET`**
(env, never a flag — flags are visible in `ps`).

Required claims: `sub`, `roles` (array, must contain at least one of `ADMIN`, `OPERATIONS`,
`READ_ONLY`), `iat`, `exp` (`exp - iat ≤ 3600`), `jti`. `alg` must equal exactly `HS256` — `none`
and any other algorithm are rejected.

There is **no `/auth/*` route** — the API never issues or revokes tokens over HTTP.
`scripts/issue-admin-token.sh` mints tokens offline (openssl HMAC, no C++ build dependency).
Revocation is an in-memory `jti` denylist loaded from `--revoked=FILE` at startup and reloaded on
`SIGHUP`.

Every route below requires a valid, non-revoked token bearing at least one of the three roles.
There is no route that needs a role narrower than "any valid admin token" — nothing here mutates,
so there is nothing to gate more tightly than read access itself.

## Routes

Every route is `GET`. Anything else on a known path is `405`; an unknown path is `404`.

### `GET /healthz` — unauthenticated

```json
{"status":"ok"}
```

### `GET /api/v1/engine/status`

Per-shard durability-plane status for every discovered instrument.

```json
{
  "source": "disk",
  "shards": [
    {
      "instrument_id": 1,
      "last_good_seq": 20,
      "segment_count": 1,
      "snapshot_count": 1,
      "newest_snapshot_seq": 20,
      "journal_bytes": 3200,
      "open_segment": true
    }
  ]
}
```

### `GET /api/v1/instruments`

Every `shard-<id>` directory found under `--journal=ROOT` (or, if none exist, the flat
pre-Spec-011 layout as id `1`). Paginated (`?limit=&after=`, snake_case envelope — see below).

```json
{"items":[{"id":1},{"id":2}],"next_cursor":null,"has_more":false}
```

### `GET /api/v1/instruments/{id}/journal/segments`

```json
{
  "items": [
    {"file_name":"seg-0000000000000000.jnl","first_seq":1,"created_counter":0,
     "bytes":1024,"header_valid":true}
  ],
  "next_cursor": null,
  "has_more": false
}
```

### `GET /api/v1/instruments/{id}/snapshots`

```json
{
  "items": [
    {"file_name":"snap-0000000000000020.snap","global_seq":20,"book_seq":20,
     "order_count":5,"bytes":360,"crc_valid":true}
  ],
  "next_cursor": null,
  "has_more": false
}
```

## Pagination contract

`?limit=N&after=K` — `after` is the last-seen numeric key of the collection (an instrument id, a
segment's `first_seq`, a snapshot's `global_seq` — every collection here is keyed by a monotonic
`u64`/`i64`, so this needs no separate opaque cursor encoding). `limit` defaults to 100, caps at
1000. Response envelope is snake_case: `{"items":[…],"next_cursor":…|null,"has_more":true|false}`.

## Six contradictions in the source planning docs, resolved fresh

`planning/05-api-design.md` and `planning/05-openapi.yaml` disagreed with each other on port, base
path, token issuance, revocation, pagination casing, and the authorization model (roles vs
scopes) — see `specs/013-rest-admin-api/spec.md`'s "The source docs are broken" section for the
full list. None of those documents were transcribed; every decision above was made fresh and is
also recorded in `specs/013-rest-admin-api/spec.md`'s "Revised scope" section and
`.claude/plans/013-rest-admin-api.md`.

## What is deliberately not here

The large CRUD surface `planning/05-*` originally described — participants, credentials, risk
limits, trading sessions, `/orders`, `/trades`, `/execution-reports`, `/audit-events` — is **not**
part of this revival and stays deferred. It has no backing store: Spec 012's audit schema
(`audit/sql/001_init.sql`) explicitly refuses to hold credentials or risk limits, by design.
A mutating endpoint would have nowhere to write.
