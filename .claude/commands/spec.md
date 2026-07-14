---
description: Pick up a spec — read it, write its plan.md and tasks.md, then implement it.
allowed-tools: Read, Write, Edit, Bash, Grep, Glob, Task
---

Begin work on spec **$ARGUMENTS** (e.g. `/spec 002`, or `/spec 002-order-lifecycle`).

Backlog:
!`ls -d specs/*/ 2>/dev/null | sed 's|specs/||;s|/||'`

## The loop

**1. Read, in this order.** Do not skip this and do not work from memory:
   - `.specify/memory/constitution.md` — the principles that govern the spec
   - `specs/<NNN>-*/spec.md` — the WHAT
   - `CLAUDE.md` — the non-negotiables and conventions
   - The relevant skill (`low-latency-cpp`, `order-book-internals`, `matching-semantics`,
     `benchmark-methodology`) — these load automatically, but confirm the right one did

**2. Clarify before planning.** Read the spec critically and list every ambiguity you find. If any
would change the design, **ask the user** — do not guess and do not silently pick. A spec that
silently means two things produces code that does neither.

**3. Write `specs/<NNN>-*/plan.md` — the HOW.** Files to create/change, the data structures, the
algorithms, the trade-offs considered and rejected. Cite the FR/NFR ids the spec claims to satisfy
and say concretely how each is met. Check the plan against the constitution before proceeding: if the
plan needs an allocation on the hot path, a lock, or a source of nondeterminism, **the plan is wrong**
— rework it, do not seek an exception.

**4. Write `specs/<NNN>-*/tasks.md` — phased, numbered, individually verifiable tasks.** Every task
that adds behavior must name the golden replay scenario or property test that proves it (NFR-37). A
task with no test is not a task.

**5. Implement, one task at a time.** Review between each. After any edit to `engine/` or `book/`,
run the `latency-reviewer` sub-agent — this is mandatory (NFR-36), not optional.

**6. Verify against the spec's stated DoD.** Actually run things:
   - `ctest --test-dir build -L unit`
   - `/replay` — byte-identical
   - `/invariants` — all four hold
   - `/bench` — no p99 regression
   - `/alloc-check` — 0 bytes/op, for any hot-path change

**7. Append an entry to `progress_report.md`** (MANDATORY RULE 2) — what, why, how, and every issue
hit along the way. Then commit, with **no `Co-Authored-By` trailer** (MANDATORY RULE 1).

The spec is done only when all of the above is true. Not when the code compiles.
