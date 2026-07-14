# velox-matching-engine — CLAUDE.md

## What this is

A low-latency, in-memory order-matching engine and mini-exchange, built entirely in C++20.
Single-threaded deterministic matching hot path, lock-free hand-rolled SPSC ring-buffer ingress,
journaled event-sourced recovery, binary order gateway, market-data feed, live visualizer.

The headline is **measured tail latency**. Every number in this repo is a real measurement on
stated hardware — never "implemented X", never an estimate.

## Where the truth lives

- **Principles (non-negotiable):** `.specify/memory/constitution.md` — read before any engine change
- **Feature specs:** `specs/NNN-name/{spec,plan,tasks}.md` — **always read the relevant spec first**
- **Project definition & backlog:** `order-matching-engine-spec.md` (root) — the authoritative source
- **Original planning docs:** `planning/*.md` — **treat with caution**, see "Known planning-doc defects" below
- **Benchmark baselines (regression gate):** `benchmarks/baselines/summary.json`
- **Hardware the numbers were measured on:** `benchmarks/baselines/hardware.md`
- **The build story so far:** `progress_report.md`

---

## MANDATORY RULE 1 — Git commit messages

**NEVER add a `Co-Authored-By:` trailer to any commit message.** Not
`Co-Authored-By: Claude <noreply@anthropic.com>`, not any other variant, not ever. It causes
failures when pushing to GitHub.

Commit messages end at the body. No trailers, no attribution footer, no "Generated with" line.

A `PreToolUse` hook (`.claude/scripts/guard-bash.sh`) blocks any `git commit` carrying such a
trailer, so this rule is enforced mechanically — but do not rely on the hook. Just never write it.

## MANDATORY RULE 2 — `progress_report.md`

`progress_report.md` is the running story of this project: what we did, why we did it, how we did
it, and what broke along the way. It is a first-class deliverable, not a changelog.

**After every meaningful change — a spec implemented, a bug fixed, a decision reversed, a tool
added — append a new entry.** Do not batch several changes into one entry after the fact, and do
not skip the entry because the change "was small".

Entry format (strict — the file is parsed by eye, keep it uniform):

```markdown
## [NNN] YYYY-MM-DD — Short imperative title

**What:** What changed, concretely. Name the files and the behavior.

**Why:** The problem this solved or the spec requirement it satisfies. Cite the spec/FR/NFR id.

**How:** The approach taken, and what was rejected. This is where the engineering reasoning goes.

**Issues:** What went wrong and how it was resolved. Omit this subsection only if nothing did.
```

Rules:
- **Append-only.** Never edit or delete a past entry. If an entry turns out to be wrong, write a
  new entry that corrects it and says so.
- Entry numbers are sequential and never reused.
- Record **real** outcomes. If a benchmark regressed or a test failed, the entry says so.
- `/progress` is the slash command that writes an entry; use it, or write the entry by hand.

---

## NON-NEGOTIABLES (the hot path is sacred)

The hot path is `engine/` and `book/`. These five are architectural law, not guidelines. See the
constitution for the full statement and the carve-outs.

1. **ZERO heap allocation on the matching hot path.** No `new`/`malloc`, no exceptions, no virtual
   dispatch, no dynamic containers that reallocate. Use object pools, flyweights over the ring, and
   pre-sized custom/open-addressing containers. Pool exhaustion produces **backpressure, not an
   allocation**.
2. **SINGLE WRITER.** The matching engine is one pinned thread. No `std::mutex`, no locks, no
   concurrent containers on the hot path. Cross-thread handoff is via the hand-rolled SPSC ring
   buffer only.
3. **NO hot-path logging.** No spdlog, no iostream, no printf on the hot path. Telemetry is via
   `alignas(64)` atomic counters and off-thread consumers only.
4. **DETERMINISM.** Same input journal ⇒ byte-identical output. Never introduce wall-clock,
   randomness, or iteration-order nondeterminism into the engine. `steady_clock` is the *only*
   permitted clock, and only for latency capture (see the constitution's carve-out).
5. **EVERY change to `engine/`/`book/` MUST keep golden replay tests green AND must not regress p99
   beyond budget.** Run `/replay` and `/bench` before calling anything done.

**Prices are scaled `int64_t`** (price × 10,000). There is no floating point anywhere in the engine.

## Performance budget (hard gate)

| Metric | Budget |
|---|---|
| p50 order-to-match | ≤ 2 µs |
| p99 order-to-match | ≤ 20 µs |
| p999 order-to-match | ≤ 100 µs |
| Throughput | ≥ 1,000,000 orders/sec (single matching thread) |
| Hot-path allocation | 0 bytes/op at steady state |
| p99 regression vs baseline | > 20% ⇒ **hard failure**, same as a failing test |

## Build / test / bench

```bash
cmake -B build -G Ninja           # configure (deps auto-fetched via FetchContent)
cmake --build build               # build

ctest --test-dir build -L unit        # unit + structural tests
ctest --test-dir build -L replay      # golden replay scenarios (byte-for-byte)
ctest --test-dir build -L invariant   # randomized property tests
ctest --test-dir build -L alloc_check # hot path allocates 0 bytes/op

./build/benchmark/velox_bench         # Google Benchmark + HdrHistogram (p50/p99/p999)
./build/benchmark/velox_alloc_check   # bytes/op on the hot path
```

## Conventions

- **C++20**, CMake + Ninja, clang-format-enforced (a `PostToolUse` hook auto-formats on write).
- Hot-path code lives in `engine/` and `book/` — **extra scrutiny applies there**. The
  `latency-reviewer` sub-agent MUST be run after editing either.
- Off-hot-path code (`gateway/`, `marketdata/`, `sequencer/`, `visualizer/`) may allocate and log
  normally. The gateway is the only component that allocates freely.
- Platform-specific code (core pinning, CPU pause, page prefault) goes behind `platform/platform.hpp`
  — never `#ifdef` in engine code. We develop on macOS-arm64; Linux-x86 is the benchmark target.
- **Tests are the deliverable.** New behavior ⇒ a golden replay scenario or a property test for it.
  A spec is not done without one.

## Definition of done (per spec)

A task is done when: code compiles clean, all tests green, the spec's stated DoD is met, `/bench`
shows no p99 regression vs baseline, (for engine changes) `/replay` is byte-identical, and
**`progress_report.md` has an entry for it**.

## Known planning-doc defects

`planning/*` was written before implementation and has real problems. Trust `order-matching-engine-spec.md`
and the `specs/` backlog over it. Specifically:

- **All five planning files are truncated mid-sentence.** `02-architecture.md` is missing its
  Security and Tech Stack sections; `04-database-design.md` is missing the snapshot binary format;
  `05-openapi.yaml` is missing its entire `components:` block and does not parse.
- **The PostgreSQL design (`04-*`) and REST admin API (`05-*`) contradict CON-8**, which forbids any
  external database. **CON-8 wins.** That work is deferred to `specs/012-*` and `specs/013-*`, is
  optional, and blocks nothing. Do not introduce a database dependency into the core.
- `03-system-design.md` has a **side bug** in its matching-loop pseudocode (resting always inserts
  into the bid book regardless of side) and **conflates `orderType` with `timeInForce`**. Do not copy
  it verbatim.
- `03-system-design.md` declares the outbound ring as SPSC but gives it **two consumers**. It needs
  either two rings or a Disruptor-style multi-consumer barrier. Resolve this in Spec 005/008.
- Journal `fsync`-per-record cannot coexist with 1M orders/sec. They only reconcile because the
  `bench` mode skips the journal — so the throughput headline is a **no-durability number** and must
  always be reported that way.
