#!/usr/bin/env bash
# Spec 009 T6: the local regression gate. Runs the engine path at the baseline's implied rate
# for 1,000,000 samples (p999 is withheld at that count -- fine, this is a p99 gate only, per
# constitution Principle 1) and checks the result against the committed baseline.
#
# Authoritative locally. Report-only in cloud CI (.github/workflows/ci.yml) -- a shared vCPU
# cannot support a 20% latency gate.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${VELOX_BUILD_DIR:-$ROOT_DIR/build}"
LOADGEN="$BUILD_DIR/benchmark/velox_loadgen"
BASELINE="$ROOT_DIR/benchmarks/baselines/summary.json"
RUN_OUT="$ROOT_DIR/benchmarks/runs/e2e_ring_steady_gate.json"

mkdir -p "$ROOT_DIR/benchmarks/runs"

if [[ ! -x "$LOADGEN" ]]; then
    echo "velox_loadgen not built at $LOADGEN -- build first." >&2
    exit 1
fi

REPORT_ONLY_FLAG=""
if [[ "${1:-}" == "--report-only" ]]; then
    REPORT_ONLY_FLAG="--report-only"
fi

"$LOADGEN" --path=engine --rate=1000000 --samples=1000000 --warmup=200000 \
    --json-out="$RUN_OUT"

python3 "$ROOT_DIR/scripts/check_regression.py" \
    --baseline "$BASELINE" --result "$RUN_OUT" $REPORT_ONLY_FLAG
