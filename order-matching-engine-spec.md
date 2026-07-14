# Order-Matching Engine — Project & Spec-Driven Build Plan

> **Codename:** `velox` (Latin *swift*). Suggested repo name `velox-matching-engine`; rename freely.
>
> **One-line pitch (resume-ready):** A low-latency, in-memory order-matching engine and mini-exchange in modern C++20 — single-threaded deterministic hot path, lock-free ingress, journaled crash recovery, a binary order gateway, a market-data feed, and a live order-book visualizer — with a measured microsecond tail-latency story from day one.
>
> **Resume axis this owns:** *Performance / real-time.* It is the showpiece for low-latency, mechanical-sympathy, and tail-latency-measurement skills, targeting low-latency / quant / HFT-adjacent / infra roles (IMC, Optiver, Jane Street–adjacent, DE Shaw, Citadel Securities, Stripe-infra, etc.).
>
> **Self-contained:** this is the only file you need for this project. It is *not* the line-by-line technical implementation — that gets written per spec, later. This is the **high-level project definition + the complete Claude Code spec-driven-development (SDD) setup + the ordered spec backlog.**

---

## 0. Locked decisions (settled before writing this doc)

| Decision | Choice | Why |
|---|---|---|
| **Language** | **C++20, single phase** | Mechanical sympathy is the whole point of this project — building it once, directly in C++, with full control over allocation, cache layout, and threading, is more honest and more efficient than building it twice. Google Benchmark/GoogleTest/HdrHistogram_c make the numbers just as provable as any managed-runtime toolchain. |
| **Scope** | **Full mini-exchange** (matching core + binary gateway + sequencer/journal recovery + market-data feed + live visualizer) | A pure matching *core* can read as "a data-structure exercise." The wider surface (protocol design, event sourcing, recovery, market data) demonstrates systems thinking and gives far more interview surface — while the low-latency core stays the headline. |
| **Demo** | **Live order-book + latency-histogram visualizer** (web) | The single most tangible thing a non-expert recruiter can grasp. It is a **read-only consumer** of the output stream — it never touches the hot path, so it costs nothing in latency credibility. |
| **Concurrency model** | **Single-threaded matching hot path** (LMAX-style), fed by a **lock-free ring buffer** | Determinism + lowest, most predictable latency. This is the architecture real exchanges (LMAX) actually use; it is also the most defensible interview answer. Settled — not re-litigated below. |
| **Order book structure** | Price levels (best-price fast) + **intrusive FIFO queue per level** + order-id → node map for O(1) cancel | Fast best-price lookup, insert, cancel, and match — the structure you defend. |

> **The honest framing rule (applies everywhere):** every headline is a *measured* number on stated hardware, never "implemented X." Correctness is *proven* (replay + property + invariant tests), not claimed.

---

## 1. What it is (plain language)

The core of a stock/crypto exchange. Buy and sell **orders** arrive; the engine maintains an **order book** and matches them by **price-time priority** (best price first, then earliest order), executing **trades** — all in-memory, optimized for the **lowest, most predictable latency**. Around that core sits a thin but real exchange: a binary **order gateway** clients connect to, a **sequencer + journal** that makes the system crash-recoverable and perfectly reproducible, a **market-data feed** that streams book updates and trades, and a **live visualizer** that shows the book and the latency distribution in real time.

## 2. The real problem / who uses it

Exchanges and trading systems must match enormous order volumes with minimal, predictable latency and exact correctness. "Design a matching engine / order book" is also one of the most common low-latency system-design interview prompts — this is the project where you'll have *built* the answer. Audience: low-latency / quant / performance / infra interviewers.

## 3. How ours differs from toy versions

Toy versions match naively, with no latency discipline and missing edge cases. Ours has: an order-book structure designed for fast insert/cancel/match; a **latency-disciplined hot path** (zero per-order allocations, no locks, single-writer); exact **price-time priority** with correct partial fills, cancels, cancel/replace, crossing books, and self-trade prevention; **deterministic event-sourced recovery**; and a **measured latency distribution (p50/p99/p999)** as the headline — captured without coordinated omission.

---

## 4. Architecture & data flow

```
   Clients
     │  binary TCP (length-prefixed frames): NewOrder / Cancel / Replace
     ▼
┌──────────────────┐   decode · validate · auth · assign client seq
│  Order Gateway   │   (Boost.Asio async I/O per connection; off the hot path)
└──────────────────┘
     │ publish command onto inbound ring (lock-free, single-producer-claim)
     ▼
┌──────────────────┐   append every command to durable journal (fsync, segments)
│   Sequencer      │   assign GLOBAL monotonic sequence  ← source of truth
└──────────────────┘
     │
     ▼
┌─────────────────────────────────────────────────────────────┐
│  MATCHING ENGINE  (single writer thread · deterministic)      │
│    Order book:                                                │
│      • Bids: price levels DESC, FIFO queue per level          │
│      • Asks: price levels ASC,  FIFO queue per level          │
│      • orderId → node map (O(1) cancel / replace)             │
│    On new order: match best opposite while crossing →         │
│      emit trades (partial fills); residual rests at its level │
│    Order types: limit · market · IOC · FOK · cancel · replace │
│    Self-trade prevention; zero-allocation hot path            │
└─────────────────────────────────────────────────────────────┘
     │
     ├──► Execution reports  ──► Gateway ──► clients
     ├──► Market-data publisher (L2/L3 incremental book + trade ticks) ──► subscribers
     └──► (state is fully rebuildable by replaying the journal)
                                  │
                                  ▼
                    ┌──────────────────────────────┐
                    │  Live Visualizer (web)        │  read-only
                    │  • animated order-book ladder │
                    │  • real-time latency histogram│
                    └──────────────────────────────┘

Recovery: on restart, replay journal from last snapshot → byte-identical state.
The SAME determinism powers reproducible correctness tests (golden replay).
```

---

## 5. The genuinely hard parts (what you defend in interviews)

1. **Data-structure design.** An order book with fast best-price lookup, insert, cancel, match. Price levels (array-indexed when ticks are bounded, else a tree map) + intrusive doubly-linked FIFO per level + `orderId → node` map for O(1) cancel/replace. Be able to justify the trade-offs vs alternatives (heaps, skip lists, plain TreeMaps).
2. **Latency discipline (the headline).** Zero allocations on the hot path (object pools, flyweight events over the ring, primitive/pooled data structures), no locks (single-writer principle), no exceptions or virtual dispatch on the hot path, no hot-path logging, mechanical sympathy (cache-friendly layout, false-sharing padding). Measure **tail** latency (p99/p999) **without coordinated omission** (HdrHistogram_c). Zero-allocation by construction from day one — there is no garbage collector to fight in the first place.
3. **Determinism & recovery (event sourcing).** Single-threaded deterministic processing + a journal of every inbound command ⇒ exact replay ⇒ crash recovery *and* reproducible tests from one mechanism.
4. **Correctness under all order types.** Exact price-time priority; correct partial fills, cancels, cancel/replace, crossing books; market/IOC/FOK semantics; self-trade prevention; no order lost or double-filled. Continuously-checked invariants (quantity conservation, sequence monotonicity, no crossed book post-match, FIFO fairness).

---

## 6. Tech stack

**C++20** (the matching thread is a pinned OS thread; gateway I/O runs on Boost.Asio), a **hand-rolled lock-free SPSC ring buffer** (cache-line padded head/tail, power-of-two sizing — single-writer handoff), custom **pool/arena allocators** and open-addressing hash maps for hot-path data structures (no heap allocation, no exceptions, no virtual dispatch on the hot path), **HdrHistogram_c** (coordinated-omission-free latency capture), **Google Benchmark** (microbenchmarks), **perf / Valgrind (callgrind)** (flamegraphs / profiling), **GoogleTest** (+ lightweight property-test helpers for invariants), **CMake** (build system), **clang-format / clang-tidy** (formatting and static analysis), **ASan/UBSan/TSan** (sanitizers), **Boost.Beast** (REST + WebSocket server), **spdlog** (off-hot-path logging), **jwt-cpp** (REST API JWT auth), **Docker**. Visualizer: lightweight web frontend (plain TS + canvas, or React) consuming the market-data + latency stream over WebSocket.

---

## 7. Headline results to *earn* (the resume bullets)

Targets — tune to your hardware and report the real numbers you measure:

- **Latency (centerpiece):** *"Median ~1–2 µs, p99 ~10–20 µs order-to-match at ~1M+ orders/sec, single-threaded — tail measured with HdrHistogram (no coordinated omission)."*
- **Correctness:** *"Matches a golden reference on N replay scenarios covering every order type and edge case (partial fills, cancels, replace, crossing books, IOC/FOK, self-trade prevention)."*
- **Recovery:** *"Journaled, event-sourced engine: killed mid-stream and restarted to byte-identical state; replay reproduces all trades deterministically."*

> **Resume line:** *Order-Matching Engine (C++20) — built a low-latency exchange core (single-threaded deterministic matching, lock-free ring ingress, journaled recovery, binary gateway, market-data feed, live visualizer); ~p99 15µs @ 1M orders/sec.*

---

# PART II — Claude Code Spec-Driven Development Setup

This is everything to put in the repo so Claude Code builds the project the SDD way: a **constitution → spec → plan → tasks → implement** loop, with deterministic guardrails so the *low-latency invariants are enforced by tooling, not hope.*

## 8. SDD workflow & tooling choice

Use **GitHub Spec Kit** (`github/spec-kit`) as the SDD backbone — it persists artifacts to Git (constitution, spec, plan, tasks), works in PR review, and survives across sessions. Initialize with:

```bash
uv tool install specify-cli --from git+https://github.com/github/spec-kit.git
specify init velox-matching-engine --ai claude
```

This scaffolds `.specify/` and `.claude/commands/` with the Spec Kit slash commands. The per-feature loop is:

```
/speckit.constitution   → .specify/memory/constitution.md   (non-negotiable principles)
/speckit.specify        → specs/NNN-name/spec.md            (WHAT we're building)
/speckit.clarify        → resolves ambiguities in the spec
/speckit.plan           → specs/NNN-name/plan.md            (HOW we're building it)
/speckit.tasks          → specs/NNN-name/tasks.md           (phased, numbered tasks)
/speckit.analyze        → cross-checks spec ↔ plan ↔ tasks for contradictions
/speckit.implement      → executes the tasks one at a time, review between each
```

> **Important:** `specify init --ai claude` does **not** generate `CLAUDE.md`. You must write it (Section 9) so Claude knows the project's vocabulary, where specs live, the non-negotiables, and the definition of done. Without it, Claude won't honor the constitution across sessions.

**Alternative / supplement:** the **Superpowers** plugin (`/plugin install superpowers@claude-plugins-official`) auto-loads an SDD workflow via a SessionStart hook — lighter weight for solo work. Spec Kit is the better fit here because you want the spec backlog living in Git as a portfolio artifact reviewers can read.

---

## 9. `CLAUDE.md` (root) — write this verbatim, then trim

Keep it lean: it's read at the start of *every* session and costs tokens every turn. Four things earn their place — architecture, build/test/bench commands, the conventions you actually enforce, and a pointer to specs — plus this project's hard non-negotiables.

```markdown
# velox-matching-engine — CLAUDE.md

## What this is
A low-latency, in-memory order-matching engine + mini-exchange, built entirely in C++20.
Single-threaded deterministic matching hot path, lock-free hand-rolled ring-buffer ingress,
journaled event-sourced recovery, binary order gateway, market-data feed, live visualizer.

## Where the truth lives
- Principles (non-negotiable): .specify/memory/constitution.md
- Feature specs: specs/NNN-name/{spec,plan,tasks}.md  ← always read the relevant spec first
- Architecture diagram + headline numbers: README.md
- Benchmark baselines (regression gate): benchmarks/baselines/*.json

## NON-NEGOTIABLES (the hot path is sacred)
1. ZERO heap allocation on the matching hot path. No `new`/`malloc` in hot-path modules,
   no exceptions, no virtual dispatch, no dynamic containers that reallocate. Use object
   pools, flyweights over the ring, and pre-sized custom/open-addressing containers.
2. SINGLE WRITER. The matching engine is one thread. No `std::mutex`, no locks, no
   concurrent containers on the hot path. Cross-thread handoff is via the hand-rolled
   SPSC ring buffer only.
3. NO hot-path logging. Telemetry is via counters / off-thread consumers only.
4. DETERMINISM. Same input journal ⇒ byte-identical output. Never introduce wall-clock,
   randomness, or iteration-order nondeterminism into the engine.
5. EVERY change to engine/book code MUST keep golden replay tests green AND must not
   regress p99 beyond the budget (see constitution). Run /bench and /replay before "done".

## Build / test / bench
- Build:        cmake --build build
- Unit+prop:    ctest --test-dir build -L unit
- Replay tests: ctest --test-dir build -L replay      (golden scenarios)
- Invariants:   ctest --test-dir build -L invariant    (randomized-schedule property tests)
- Benchmarks:   ./build/bench/velox_bench               (Google Benchmark + HdrHistogram_c)
- Alloc check:  ./build/bench/velox_alloc_check          (asserts ~0 bytes/op on hot path)

## Conventions
- C++20, CMake, clang-format-formatted (a hook auto-formats on write).
- Hot-path code lives in `engine/` and `book/` modules — extra scrutiny applies there.
- Off-hot-path (gateway, market-data, visualizer) may allocate/log normally.
- Tests are the deliverable. New behavior ⇒ a replay scenario or property test for it.

## Definition of done (per spec)
A task is done when: code compiles, all tests green, the spec's stated DoD is met,
/bench shows no p99 regression vs baseline, and (for engine changes) /replay is byte-identical.
```

---

## 10. The constitution (`.specify/memory/constitution.md`) — principles to encode

Generate with `/speckit.constitution`, then ensure it contains these non-negotiable principles (the constitution governs every downstream spec/plan/task):

1. **Performance budget is a hard gate.** A change that regresses p99 beyond the committed budget is rejected, the same way a failing test is. Budget lives in `benchmarks/baselines/`.
2. **Correctness is proven, not claimed.** Every order-type behavior and edge case has a golden replay scenario and/or a property test. The book invariants are asserted after every operation in tests.
3. **Determinism is mandatory** in the engine. No wall-clock, no randomness, no nondeterministic iteration in the hot path.
4. **Single-writer / zero-allocation hot path** (as in CLAUDE.md) — architectural law, not a guideline.
5. **Thin vertical slice first, then deepen.** Every spec must be demoable end-to-end before the next begins.
6. **Measure from day one.** A minimal latency harness exists from Spec 001; you never optimize blind.

> Keep it strict but not *so* strict it paralyzes downstream work (an over-strict constitution makes every task balloon). These six are the right altitude.

---

## 11. Custom slash commands (`.claude/commands/*.md`)

Beyond the Spec Kit `/speckit.*` commands, add these **project-specific** commands. Each is a markdown file with a short frontmatter + an instruction body; `$ARGUMENTS` expands to whatever the user typed after the command, and `` !`cmd` `` runs a shell command at load time and inlines its output.

| Command | File | What it does |
|---|---|---|
| `/bench [scenario]` | `commands/bench.md` | Run Google Benchmark + load generator, capture HdrHistogram, regenerate the latency plot, **diff p50/p99/p999 + throughput against `benchmarks/baselines/`**, and report regressions. |
| `/replay <scenario>` | `commands/replay.md` | Run a deterministic golden-replay test and assert produced trades match the reference **byte-for-byte**. |
| `/invariants` | `commands/invariants.md` | Run the property-test suite (quantity conservation, sequence monotonicity, no crossed book, FIFO fairness) over randomized schedules. |
| `/alloc-check` | `commands/alloc-check.md` | Run the allocation profiler and **fail if the hot path allocates** more than the threshold (target ~0 bytes/op). |
| `/recover-test` | `commands/recover-test.md` | Kill the engine mid-stream, restart, replay the journal, and assert byte-identical recovered state. |
| `/profile [scenario]` | `commands/profile.md` | Run async-profiler and produce a flamegraph of the hot path to find the next bottleneck. |
| `/perf-baseline` | `commands/perf-baseline.md` | Promote the current benchmark numbers to the committed baseline (explicit, deliberate — guarded). |

**Example — `commands/bench.md`:**

```markdown
---
description: Benchmark the engine and gate against the committed baseline.
allowed-tools: Bash, Read
---
Run the full benchmark suite for scenario "$ARGUMENTS" (default: all):

Current baseline: !`cat benchmarks/baselines/summary.json`

1. Run `./build/bench/velox_bench` and the load generator for the scenario.
2. Parse the HdrHistogram output for p50, p99, p999, and throughput.
3. Compare to the baseline above. If p99 regressed by more than the budget, FAIL loudly
   and summarize which scenario regressed and by how much.
4. Regenerate benchmarks/plots/latency-<scenario>.png.
5. Do NOT promote a new baseline — that's /perf-baseline, a deliberate step.
```

---

## 12. Skills (`.claude/skills/<name>/SKILL.md`)

Skills are model-invoked: Claude loads them automatically based on their `description` when the task matches. Use them to inject the **domain rules** Claude must respect so it doesn't reintroduce allocations/locks while editing.

| Skill | Triggers when… | Contents (high level) |
|---|---|---|
| `low-latency-cpp` | editing hot-path code in `engine/`/`book/` | The zero-allocation rulebook: no `new`/`malloc`/exceptions/virtual dispatch/logging on hot path; object pools & flyweights; RAII; custom pooled/open-addressing containers; false-sharing padding; single-writer; mechanical-sympathy checklist. |
| `order-book-internals` | touching the order book | The exact data structure + the invariants that must hold after every op; how cancel/replace mutate the intrusive FIFO + id-map; price-level lifecycle. |
| `matching-semantics` | implementing/altering matching | Precise price-time priority rules; limit/market/IOC/FOK semantics; partial fills; crossing-book handling; self-trade prevention — the correctness spec Claude consults. |
| `benchmark-methodology` | writing/altering benchmarks | How to measure honestly: Google Benchmark warmup/iterations; HdrHistogram_c + **coordinated-omission avoidance**; pin to stated hardware; baseline JSON format; what numbers mean. |

> The Spec Kit plugin also ships a `spec-driven-development` skill that auto-guides the SDD methodology — keep it.

**Example frontmatter — `skills/low-latency-cpp/SKILL.md`:**

```markdown
---
name: low-latency-cpp
description: Rules for editing the matching-engine hot path (engine/, book/). Use whenever
  writing or modifying code in those modules to avoid reintroducing allocations, locks,
  exceptions, virtual dispatch, or logging on the critical path.
---
(body: the zero-allocation rulebook + mechanical-sympathy checklist)
```

---

## 13. Sub-agents (`.claude/agents/*.md`)

Sub-agents run in their own context window with a scoped tool list — ideal for the verify/measure steps so they don't bloat the main session. Use "PROACTIVELY" / "MUST BE USED" in the description to encourage delegation.

| Sub-agent | Tools | Role |
|---|---|---|
| `latency-reviewer` | Read, Grep, Bash | **MUST BE USED after editing hot-path code.** Greps the diff for `new`/`malloc`, `throw`, `virtual`, `std::mutex`/`std::lock_guard`, `std::cout`/`std::cerr`/logging calls in `engine/`/`book/`; runs a clang-tidy pass; runs `/alloc-check`; reports violations. |
| `correctness-verifier` | Read, Grep, Bash | Runs replay + property + invariant suites, parses failures, and reports the minimal failing scenario. Use proactively after any engine change. |
| `benchmark-runner` | Read, Bash | Runs `/bench`, parses HdrHistogram, compares to baseline, reports regressions. Isolated so heavy output doesn't pollute the main context. |
| `spec-author` | Read, Write | Helps draft and clarify specs from this backlog; keeps spec ↔ plan ↔ tasks consistent. |

**Example — `.claude/agents/latency-reviewer.md`:**

```markdown
---
name: latency-reviewer
description: Expert low-latency reviewer. MUST BE USED immediately after editing any code in
  engine/ or book/. Catches hot-path allocations, exceptions, virtual dispatch, locks, and
  logging before they land.
tools: Read, Grep, Bash
model: inherit
---
You are a low-latency C++ reviewer. For the changed files in engine/ and book/:
1. Flag any `new`/`malloc` (except in clearly off-hot-path init), `throw`/exceptions,
   `virtual` dispatch, `std::mutex`/`std::lock_guard`/other locking primitives, dynamic
   container reallocation, or logging/iostream calls on the hot path.
2. Run the alloc-check benchmark target and report bytes/op; anything above threshold is a
   failure. Run clang-tidy and report new warnings in changed files.
3. Report findings as a short pass/fail list with file:line. Do not rewrite code yourself.
```

---

## 14. Hooks (`.claude/settings.json`)

Hooks are **deterministic** guardrails (unlike prompt instructions, they run the same way every time). This is where you make the hot-path invariants impossible to forget. Hooks are configured under the top-level `hooks` key; events fire with a `matcher` and one or more handlers.

| Event | Matcher | Handler → effect |
|---|---|---|
| `PostToolUse` | `Edit\|Write` | Run clang-format on the written file, then a fast `hot-path-lint.sh` that scans `engine/`/`book/` diffs for forbidden patterns (allocation, locks, exceptions, virtual dispatch, logging) and warns Claude. |
| `PreToolUse` | `Bash` | Block destructive commands (`rm -rf`, force-push to main, deleting `benchmarks/baselines/`). Exit 2 + `permissionDecision: "deny"`. |
| `SessionStart` | — | Inject context: the active spec, the current p99 baseline numbers, and a reminder of the non-negotiables. |
| `Stop` | — | (prompt handler) Verify that engine changes in this turn were accompanied by green `/replay` + `/invariants`; if not, remind before finishing. |

**`.claude/settings.json` (illustrative):**

```json
{
  "hooks": {
    "PostToolUse": [
      {
        "matcher": "Edit|Write",
        "hooks": [
          { "type": "command", "command": "jq -r '.tool_input.file_path' | xargs -r ./.claude/scripts/format-and-lint.sh", "timeout": 30 }
        ]
      }
    ],
    "PreToolUse": [
      {
        "matcher": "Bash",
        "hooks": [
          { "type": "command", "command": "./.claude/scripts/guard-bash.sh", "timeout": 10 }
        ]
      }
    ],
    "SessionStart": [
      {
        "hooks": [
          { "type": "command", "command": "./.claude/scripts/inject-context.sh" }
        ]
      }
    ]
  }
}
```

> `guard-bash.sh` reads the tool JSON on stdin, inspects `tool_input.command`, and exits 2 to block. `format-and-lint.sh` runs clang-format on the file and runs the hot-path scanner. Hooks run with your privileges — review them before use, and commit them so the guardrails travel with the repo.

---

## 15. MCP servers (`.mcp.json`) — keep this minimal on purpose

This project is local, offline, and latency-obsessed; bolting on MCP servers it doesn't need would be the wrong signal. Add only:

- **GitHub MCP** — issues / PRs, so the spec backlog maps to tracked issues (pairs with Spec Kit's `taskstoissues`) and PR review runs against the constitution.
- **(Optional) a browser MCP** (Playwright/Puppeteer) — only for end-to-end testing the **visualizer** UI, never the engine.

> Deliberately *not* adding database/cloud/data-warehouse MCPs — the engine is in-memory and self-contained. "I didn't add tools the system didn't need" is itself a mature engineering signal in an interview. MCP servers run third-party code with your privileges; vet anything you add.

---

## 16. Plugins / marketplaces

Plugins bundle commands + agents + skills + hooks + MCP into one installable, versioned unit. Recommended:

- **Spec Kit** (the SDD backbone — see Section 8).
- **`pr-review-toolkit@claude-plugins-official`** — multi-facet PR review (bug detection, convention compliance, history) that can check changes against your constitution.
- **`security-guidance`** (official) — reviews each change for common vulnerabilities (relevant for the gateway / protocol parsing / fuzzing surface).
- **`plugin-dev`** (official) — *optional flourish:* once your `.claude/` (commands + agents + skills + hooks) is solid, package it as a private plugin. "I packaged my engine's low-latency guardrails as a reusable Claude Code plugin" is a nice extra line, and it scaffolds the `.claude-plugin/plugin.json` manifest for you (`claude plugin validate` before publishing).

Install pattern:
```bash
/plugin marketplace add <owner/repo>      # add a marketplace
/plugin install <plugin>@<marketplace>    # install a plugin
```

---

# PART III — The Ordered Spec Backlog (high-level)

Each item becomes a Spec Kit feature (`/speckit.specify` → `specs/NNN-name/`). Listed in build order (thin slice first, then deepen; hardest/most-optional last). The exact technical design and tasks are written per-spec, later. Each carries a one-line scope, its **Definition of Done (DoD)**, and the headline it advances.

> **Spec 000 — Constitution.** Establish `.specify/memory/constitution.md` (Section 10) and `CLAUDE.md` (Section 9). **DoD:** principles, performance budget, and non-negotiables committed; a minimal Google Benchmark latency harness scaffolded so every later slice is measurable from day one.

### Phase A — Correct core

**Spec 001 — Core limit order book + price-time matching (thin vertical slice).**
In-memory, single instrument, limit orders only; maintain bids/asks with FIFO per level; match crossing orders and emit trades; residual rests.
**DoD:** a known order sequence replays to a golden trade output exactly; minimal latency harness prints p50/p99 for the matching call. *(Advances: correctness + first latency number.)*

**Spec 002 — Full order lifecycle & types.**
Market, IOC, FOK, cancel, cancel/replace (modify); partial fills; crossing-book handling; **self-trade prevention.**
**DoD:** a golden replay scenario + property test exists for every order type and edge case; no order lost or double-filled. *(Advances: correctness breadth.)*

**Spec 003 — Order-book invariants & property-based test harness.**
Encode and continuously assert: quantity conservation, sequence monotonicity, no crossed book after matching, FIFO fairness within a price level.
**DoD:** invariants hold after every operation across thousands of randomized schedules (property-based tests). *(Advances: provable correctness — the "CORRECTNESS IS THE DELIVERABLE" claim.)*

### Phase B — Latency engineering (the headline)

**Spec 004 — Zero-allocation, single-writer hot path.**
Object pooling, flyweight order/trade events, custom pool/arena allocators and open-addressing hash maps; remove all hot-path allocations, locks, and logging; cache-friendly layout + false-sharing padding.
**DoD:** allocation profiler shows ~0 bytes/op on the hot path; **all Phase-A tests still byte-identical** (latency work changed *nothing* about results). *(Advances: the latency story.)*

**Spec 005 — Hand-rolled SPSC ring-buffer ingress & threading model.**
Lock-free ring feeds the single matching thread; gateway (producer) and publishers (consumers) run off-thread; backpressure defined.
**DoD:** sustained throughput ≥ target; end-to-end (ring → match) latency measured; no contention on the hot path. *(Advances: throughput + architecture credibility.)*

### Phase C — Real exchange surface

**Spec 006 — Sequencer + journal + deterministic recovery (event sourcing).**
Append every inbound command to a durable, segmented journal (fsync) with a global monotonic sequence; periodic snapshots; rebuild exact state by replay on restart.
**DoD:** kill the engine mid-stream, restart, replay → **byte-identical** recovered state; replay reproduces all trades deterministically. *(Advances: the recovery headline + systems-thinking surface.)*

**Spec 007 — Binary wire protocol + order gateway.**
Length-prefixed binary protocol (new/cancel/replace + execution reports); decode/validate/auth; Boost.Asio async I/O / thread-per-connection handling; backpressure to clients.
**DoD:** protocol round-trip tests; malformed/hostile input rejected; a fuzz pass finds no crash. *(Advances: protocol-design + robustness surface.)*

**Spec 008 — Market-data publisher (L2/L3 + trade ticks).**
Incremental order-book updates (price-level aggregates and/or per-order) + a trade-tick stream to subscribers.
**DoD:** a subscriber can reconstruct the book identically from the feed alone. *(Advances: market-data / streaming surface.)*

### Phase D — Proof & demo

**Spec 009 — Benchmark harness & latency methodology (formalized).**
Google Benchmark microbenchmarks + a load generator + HdrHistogram (coordinated-omission-free); committed baseline JSON; a regression gate wired into `/bench` and CI; plots.
**DoD:** reproducible p50/p99/p999 + throughput on stated hardware; CI fails on p99 regression; the latency distribution plot is generated. *(Advances: **this is where the headline numbers are produced and locked.**)*

**Spec 010 — Live visualizer (web).**
Read-only consumer of the market-data + latency stream: animated order-book ladder + real-time latency histogram, fed by a replayed or live session over WebSocket.
**DoD:** the demo renders the book updating and the latency histogram live; clearly decoupled from the hot path. *(Advances: the recruiter-facing demo.)*

### Phase E — Depth

**Spec 011 — Multi-instrument & sharding (optional depth).**
One matching thread per instrument (or sharded engines), with isolation and aggregate throughput.
**DoD:** N instruments run isolated; aggregate throughput scales; per-instrument determinism preserved. *(Advances: scale surface.)*

---

## 17. Interview-defense cheat sheet (what this project lets you answer)

- *"Design an order book / matching engine."* — You built one; walk the data structure and price-time priority.
- *"Why single-threaded?"* — Determinism + no lock contention + predictable tail latency; cite LMAX. Scale by sharding per instrument (Spec 011).
- *"How do you keep the hot path fast?"* — Zero allocation, single-writer, no locks/exceptions/virtual dispatch/logging, mechanical sympathy; enforced by hooks + the `latency-reviewer` agent, *measured* by the alloc profiler.
- *"How do you measure latency honestly?"* — HdrHistogram, coordinated-omission avoidance, p99/p999 not averages, pinned hardware, committed baselines.
- *"How is it correct?"* — Golden replay + property/invariant tests; determinism makes bugs reproducible.
- *"How does it survive a crash?"* — Event-sourced journal + snapshots; replay to byte-identical state.

## 18. What to ship in the repo (deliverables checklist)

- `README.md` with the **architecture diagram**, the **latency distribution plot** (centerpiece), throughput numbers, and the recovery story.
- The `specs/` backlog (this is itself a portfolio artifact — reviewers can see you work spec-first).
- `benchmarks/` with baselines, plots, and a one-command `/bench`.
- The full test surface: `replayTest`, `invariantTest`, recovery test.
- The `.claude/` setup (commands, agents, skills, hooks) committed — guardrails travel with the repo.
- A short **design writeup**: the order-book structure choice and the single-writer rationale.

---

*Build order rationale: correctness core first (001–003) so there's a provable base; latency discipline next (004–005) since it must not change results; the real exchange surface (006–008) to show systems thinking; proof + demo (009–010) to produce the recruiter-facing artifacts; depth (011) last as an independent, high-signal bonus that blocks nothing.*
