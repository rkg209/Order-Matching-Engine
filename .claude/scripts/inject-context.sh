#!/usr/bin/env bash
# SessionStart hook: inject the state Claude must not start a session without —
# the current performance baseline, the active spec, and the non-negotiables.
#
# stdout becomes additional context for the session.

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

echo "=== VELOX SESSION CONTEXT ==="
echo ""

# --- Current committed performance baseline (the regression gate) -------------
BASELINE="$REPO_ROOT/benchmarks/baselines/summary.json"
if [[ -f "$BASELINE" ]]; then
  echo "Committed performance baseline (benchmarks/baselines/summary.json):"
  if command -v jq >/dev/null 2>&1; then
    jq -r '"  p50=\(.p50_ns)ns  p99=\(.p99_ns)ns  p999=\(.p999_ns)ns  throughput=\(.throughput_ops_sec)/s"' \
      "$BASELINE" 2>/dev/null || sed 's/^/  /' "$BASELINE"
  else
    sed 's/^/  /' "$BASELINE"
  fi
  echo "  A change that regresses p99 by >20% vs this is a HARD FAILURE (constitution P1)."
else
  echo "No committed baseline yet (benchmarks/baselines/summary.json missing)."
  echo "  The FIRST real benchmark run defines it — via /perf-baseline, never by hand."
fi
echo ""

# --- Which spec is in flight --------------------------------------------------
if [[ -d "$REPO_ROOT/specs" ]]; then
  echo "Spec backlog status:"
  for d in "$REPO_ROOT"/specs/*/; do
    [[ -d "$d" ]] || continue
    name=$(basename "$d")
    if [[ -f "$d/tasks.md" ]];      then status="IN PROGRESS (has tasks.md)"
    elif [[ -f "$d/plan.md" ]];     then status="planned"
    else                                 status="backlog"
    fi
    printf '  %-32s %s\n' "$name" "$status"
  done
  echo ""
fi

# --- The rules that must never be forgotten -----------------------------------
cat <<'EOF'
NON-NEGOTIABLES (full text: .specify/memory/constitution.md):
  1. ZERO heap allocation on the hot path (engine/, book/). Pools + flyweights only.
  2. SINGLE WRITER. One pinned matching thread. No locks on the hot path.
  3. NO hot-path logging. Atomic counters and off-thread consumers only.
  4. DETERMINISM. No wall-clock, no randomness. (steady_clock IS allowed, for latency capture only.)
  5. Golden replay must stay byte-identical and p99 must not regress before anything is "done".

MANDATORY RULE 1: NEVER put a `Co-Authored-By:` trailer in a git commit message.
MANDATORY RULE 2: Append an entry to progress_report.md after every meaningful change.

Prices are scaled int64_t (x10000). No floating point in the engine, ever.
EOF
echo ""
echo "=== END VELOX SESSION CONTEXT ==="
