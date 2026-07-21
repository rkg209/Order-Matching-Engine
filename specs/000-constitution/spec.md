# Spec 000 — Constitution, guardrails, and a latency harness from day one

**Status:** ✅ COMPLETE (2026-07-14) · **Phase:** A — Correct core · **Depends on:** nothing

## Scope

Establish the rules and the tooling that enforce them, *before* any engine code exists — and scaffold
a minimal latency harness so that every later slice is measurable from its first commit.

## Non-goals

- No matching logic. No order book. That is Spec 001.
- No gateway, journal, or market data.

## Why this comes first

Two reasons, and both are about what happens if it does not.

**The performance budget must precede the code.** A budget written after the fact is not a budget; it
is a description of whatever the code happens to do. p99 ≤ 20 µs is a constraint only while it is
still possible to fail it.

**The guardrails must be mechanical, not aspirational.** NFR-36 requires that hot-path modules be
scanned after *every* edit. An instruction in a prompt is advisory and decays over a long session; a
`PostToolUse` hook runs identically every time, including the time you forgot it existed. The
non-negotiables are only real if something other than memory enforces them.

## Deliverables

| Artifact | Purpose |
|---|---|
| `.specify/memory/constitution.md` | The six governing principles + the hard numeric budgets |
| `CLAUDE.md` | Session rules: non-negotiables, build commands, conventions, the two mandatory rules |
| `progress_report.md` | The append-only development story (what / why / how / issues) |
| `.claude/commands/*` (9) | `/bench` `/replay` `/invariants` `/alloc-check` `/recover-test` `/profile` `/perf-baseline` `/spec` `/progress` |
| `.claude/skills/*` (4) | `low-latency-cpp` · `order-book-internals` · `matching-semantics` · `benchmark-methodology` |
| `.claude/agents/*` (4) | `latency-reviewer` · `correctness-verifier` · `benchmark-runner` · `spec-author` |
| `.claude/settings.json` + `.claude/scripts/*` | Hooks: auto-format + hot-path lint on write; Bash guard; session-start context |
| `specs/` | This backlog (CON-11: a committed portfolio artifact) |

## Definition of Done

- [x] The six principles and the performance budget are committed and unambiguous.
- [x] The hot-path linter **actually catches** a planted violation (`new`, `std::mutex`, `std::cout`,
      `push_back`) and **does not** flag `steady_clock` — the constitution's carve-out.
- [x] The Bash guard **actually blocks** a `git commit` carrying a `Co-Authored-By` trailer, and
      **fails closed** on a malformed payload rather than waving it through.
- [x] The backlog exists, with every spec's scope and DoD stated.
- [x] A minimal Google Benchmark + HdrHistogram latency harness is scaffolded — *landed with Spec 001,
      because there is nothing to measure until there is a matching call to measure.*

## Requirements satisfied

- **NFR-35** — clang-format on every write → `PostToolUse` hook running `format-and-lint.sh`.
- **NFR-36** — hot-path modules scanned after every edit → `hot-path-lint.sh` + the `latency-reviewer`
  sub-agent.
- **CON-11** — `specs/` committed, never deleted.
- **CON-12** — `.claude/` committed; the guardrails travel with the repo.
- **Principle 6** — measure from day one; the harness is scaffolded before it is needed.

## Decisions recorded here

1. **CON-8 wins over the Postgres design.** The core is in-memory and journal-only. Specs 012/013 hold
   the deferred database and REST work; nothing depends on them. See `progress_report.md` [003].
2. **`steady_clock` is carved out** of the determinism ban, for latency capture only. Without this the
   tooling would flag its own measurement apparatus.
3. **The throughput headline is a no-durability number.** `fsync`-per-record and 1M orders/sec are
   physically irreconcilable; they coexist only because `bench` mode skips the journal. This must be
   stated every time the number is reported.
4. **Hand-rolled SDD, not GitHub Spec Kit.** Same file layout, no external tool dependency.
5. **Dependencies via CMake `FetchContent`** (not vcpkg, not Conan) so `cmake -B build` works on a
   clean machine. The planning docs never decided this.

## Verification

```bash
.claude/scripts/hot-path-lint.sh <file with a planted violation>   # must flag
printf '{"tool_input":{"command":"git commit -m \"x Co-Authored-By: y\""}}' \
  | .claude/scripts/guard-bash.sh; echo $?                         # must be 2
git log --format='%B' | grep -i co-authored-by                     # must be empty
```
