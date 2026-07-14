# Spec 012 — PostgreSQL audit tier

**Status:** ⛔ DEFERRED — optional · **Blocks:** nothing · **Nothing may depend on this**

> ⚠️ **This spec contradicts CON-8, and CON-8 wins.** It is preserved because real design work went into
> it and it may be wanted later — not because it is approved. **Do not implement it without an explicit
> decision to overturn the resolution below.**

## The contradiction

`planning/01-requirements.md` **CON-8** states:

> *"No external database or persistent store is used for the engine's primary state. The system is
> in-memory; the journal is the sole durability mechanism."*

`planning/04-database-design.md` and `planning/04-database-schema.sql` specify **PostgreSQL 16** with
roughly fourteen tables, partitioning, migrations, and an audit-writer service.

These cannot both be true. **Resolution (2026-07-14): CON-8 wins.** The core stays in-memory and
journal-only. Recorded in `progress_report.md` [003].

**Why CON-8 wins:** the entire pitch of this project is a self-contained, in-memory, microsecond-latency
engine whose sole durability mechanism is an event-sourced journal. That story is coherent, defensible,
and it is what makes the recovery narrative interesting. Bolting a relational database onto it does not
add a capability the engine needs — the journal *is* the source of truth — and it dilutes the very thing
the project is selling. The root spec makes the same call explicitly, in the MCP section: *"the engine is
in-memory and self-contained… I didn't add tools the system didn't need is itself a mature engineering
signal."*

## If this is ever revived, the source docs are NOT trustworthy

Do not transcribe them. They would need redesign, not implementation:

1. **The `.md` and the `.sql` define two different, mutually incompatible schemas.**
   - `04-database-design.md`: **singular** table names, **one** `velox` schema —
     `velox.participant`, `velox.instrument`, `velox.order_audit`, `velox.trade`, `velox.risk_limit`,
     `velox.journal_segment`, `velox.snapshot_record`, `velox.audit_event`, …
   - `04-database-schema.sql`: **plural** names, **three** schemas (`velox`, `velox_audit`,
     `velox_telemetry`) — `velox.participants`, `velox.orders`, `velox_audit.journal_commands`,
     `velox.book_snapshots`, `velox.trade_ticks`, … It **omits** credentials, risk limits, trading
     sessions, and audit events entirely, while **adding** tables the design doc never mentions.
   - They are not two views of one design. They are two designs.
2. **`04-database-schema.sql` is truncated mid-DDL** (it stops inside `CONSTRAINT pk_` in
   `velox.trade_ticks`) and begins with a literal ```` ```sql ```` markdown fence — **it will not load.**
3. **Three different components are named as the owner** of the audit rows across the two documents:
   an `audit-writer` service (which does not exist in `03-system-design.md`'s service list), the
   `ExecutionReportRouter`, and the `Sequencer`. Ownership is undefined.
4. **`velox_audit.journal_commands` mirrors the entire journal into Postgres**, written *by the
   sequencer after fsync*. This puts a **database write in the sequencer's path** — directly
   contradicting the tier model in the very same document, which insists the database is never on the
   hot path and that the filesystem journal is the sole durability layer. It is self-refuting.
5. **Immutability contradiction:** the design doc calls `order_audit` immutable and append-only; the SQL
   header says `orders` is updated in place.

## If revived, the shape it should take

Strictly a **Tier 3 audit and admin store**, never on the hot path and never on the durability path:

- The journal remains the **sole** source of truth. Postgres is a **derived, lossy, queryable view** —
  if it burns down, nothing is lost that matters.
- Written **asynchronously**, by a single clearly-named consumer of the outbound ring, which may lag
  arbitrarily far behind without the engine noticing or caring.
- **The engine must not know Postgres exists.** No header, no link dependency, no build coupling.
- Purpose is answering questions humans ask *afterwards* — what did participant 7 do yesterday? — not
  anything the engine needs to run.

## Definition of Done (if revived)

- [ ] An explicit, recorded decision overturning the CON-8 resolution.
- [ ] A **single** coherent schema replacing the two contradictory ones.
- [ ] Proven: engine p50/p99/p999 **unchanged** with the audit tier running vs not. Measured, not asserted.
- [ ] Proven: killing Postgres mid-stream does **not** affect the engine, and does not lose an order.
