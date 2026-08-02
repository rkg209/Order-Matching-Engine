#!/usr/bin/env bash
# Spec 011 T8 / DoD: "aggregate throughput scales with instrument count -- measure it, show the
# curve." Sweeps N=1..K shards through velox_loadgen --path=engine --shards=N --rate=max,
# collects each run's SHARD_SCALING summary line, and writes
# benchmarks/shard_scaling.csv (shards,aggregate_ops_sec,worst_shard_p50_ns,worst_shard_p99_ns,
# worst_shard_p999_ns,oversubscribed) plus a scaling-efficiency column
# (aggregate(N) / (N * aggregate(1))).
#
# K defaults to hardware_concurrency/3 (velox_loadgen needs 3 threads/shard -- sender + pinned
# matching + reader, benchmark/velox_loadgen.cpp's runSharded() doc) -- past that the curve
# measures the scheduler timeslicing real cores, not the engine's per-shard isolation. This
# script does NOT decide whether the curve is "good"; per the spec, a curve that does not scale
# roughly linearly is itself the finding, not a reason to fail the run.
#
# It does NOT write benchmarks/baselines/summary.json -- that file is modified only by the
# deliberate /perf-baseline command (DR-7).

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${VELOX_BUILD_DIR:-$ROOT_DIR/build}"
LOADGEN="$BUILD_DIR/benchmark/velox_loadgen"
OUT_CSV="${VELOX_SHARD_SCALING_CSV:-$ROOT_DIR/benchmarks/shard_scaling.csv}"
# Deliberately small defaults. This dev box has no core isolation (platform::supportsCoreIsolation()
# is false on macOS -- CLAUDE.md), so unpaced --rate=max sends can transiently outrun the
# reader/record pipeline and queue up; a large sample count then measures OS scheduling noise for
# minutes instead of the engine. Bump these via env vars on a Linux box with core isolation, which
# is where the REAL curve belongs (specs/011-multi-instrument/spec.md's "Honest limits").
SAMPLES="${VELOX_SHARD_SCALING_SAMPLES:-3000}"
WARMUP="${VELOX_SHARD_SCALING_WARMUP:-300}"

if [[ ! -x "$LOADGEN" ]]; then
    echo "velox_loadgen not built at $LOADGEN -- build first." >&2
    exit 1
fi

HW_CONCURRENCY="$(python3 -c 'import os; print(os.cpu_count() or 1)')"
DEFAULT_K=$(( HW_CONCURRENCY / 3 ))
if (( DEFAULT_K < 1 )); then DEFAULT_K=1; fi
K="${VELOX_SHARD_SCALING_MAX_N:-$DEFAULT_K}"

mkdir -p "$(dirname "$OUT_CSV")"
echo "shards,aggregate_ops_sec,worst_shard_p50_ns,worst_shard_p99_ns,worst_shard_p999_ns,oversubscribed,scaling_efficiency" > "$OUT_CSV"

echo "=== shard_scaling: sweeping N=1..$K (hardware_concurrency=$HW_CONCURRENCY) ==="

BASE_RATE=""
declare -a SHARDS_LIST AGG_LIST P50_LIST P99_LIST P999_LIST OVER_LIST

for (( n=1; n<=K; n++ )); do
    echo
    echo "--- shards=$n ---"
    LINE="$("$LOADGEN" --path=engine --shards="$n" --rate=max --workload=dense \
             --samples="$SAMPLES" --warmup="$WARMUP" | grep '^SHARD_SCALING ')"
    echo "$LINE"

    AGG=$(echo "$LINE" | grep -oE 'aggregate_ops_sec=[0-9.eE+-]+' | cut -d= -f2)
    P50=$(echo "$LINE" | grep -oE 'worst_p50_ns=[0-9-]+' | cut -d= -f2)
    P99=$(echo "$LINE" | grep -oE 'worst_p99_ns=[0-9-]+' | cut -d= -f2)
    P999=$(echo "$LINE" | grep -oE 'worst_p999_ns=[0-9-]+' | cut -d= -f2)
    OVER=$(echo "$LINE" | grep -oE 'oversubscribed=[01]' | cut -d= -f2)

    if [[ "$n" -eq 1 ]]; then
        BASE_RATE="$AGG"
    fi
    EFF=$(python3 -c "base=float('$BASE_RATE'); agg=float('$AGG'); n=$n; print(f'{(agg/(n*base)) if base > 0 else 0:.4f}')")

    echo "$n,$AGG,$P50,$P99,$P999,$OVER,$EFF" >> "$OUT_CSV"

    SHARDS_LIST+=("$n"); AGG_LIST+=("$AGG"); P50_LIST+=("$P50"); P99_LIST+=("$P99")
    P999_LIST+=("$P999"); OVER_LIST+=("$OVER")
done

echo
echo "=== shard_scaling curve (written to $OUT_CSV) ==="
printf "%-8s %-18s %-14s %-14s %-10s\n" "shards" "aggregate_ops_sec" "worst_p99_ns" "efficiency" "oversub"
for (( i=0; i<${#SHARDS_LIST[@]}; i++ )); do
    n="${SHARDS_LIST[$i]}"
    agg="${AGG_LIST[$i]}"
    p99="${P99_LIST[$i]}"
    over="${OVER_LIST[$i]}"
    eff=$(python3 -c "base=float('$BASE_RATE'); agg=float('$agg'); n=$n; print(f'{(agg/(n*base)) if base > 0 else 0:.4f}')")
    printf "%-8s %-18s %-14s %-14s %-10s\n" "$n" "$agg" "$p99" "$eff" "$over"
done

echo
echo "Honest limit (Spec 011): scaling is bounded by physical cores. Past N=$K on this box"
echo "(hardware_concurrency=$HW_CONCURRENCY, 3 threads/shard) the harness itself reports"
echo "OVERSUBSCRIBED and the curve measures the scheduler, not per-shard isolation -- see"
echo "specs/011-multi-instrument/spec.md's 'Honest limits to state'."
