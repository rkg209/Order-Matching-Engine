#!/usr/bin/env bash
# Spec 010 T8 / DoD: "provably decoupled -- measure it, don't assert it." Two runs of the engine
# path at the gate's rate:
#
#   A: velox_loadgen --path=engine, no visualizer attached at all.
#   B: same, plus --md-port/--stats-port, velox_viz attached, AND a browser-equivalent WebSocket
#      client connected and reading -- so this measures the actual demo configuration, not just
#      "the ports were open".
#
# Compares B's p99 against A's p99 with the SAME regression-gate script the committed-baseline
# gate uses (scripts/check_regression.py) -- A's own result JSON is reused as B's "baseline", so
# this is exactly a p99-regression check, just against a same-run reference instead of a
# committed one. Prints both p99s either way, and both runs' rate_sustained/pool_exhausted, since
# a run that did not sustain rate cannot support the decoupling claim.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${VELOX_BUILD_DIR:-$ROOT_DIR/build}"
LOADGEN="$BUILD_DIR/benchmark/velox_loadgen"
VIZ="$BUILD_DIR/apps/velox_viz"
RUNS_DIR="$ROOT_DIR/benchmarks/runs"
RATE="${VELOX_VIZ_DECOUPLING_RATE:-1000000}"
SAMPLES="${VELOX_VIZ_DECOUPLING_SAMPLES:-1000000}"
MD_PORT=19501
STATS_PORT=19502
VIZ_PORT=19580

mkdir -p "$RUNS_DIR"

if [[ ! -x "$LOADGEN" ]]; then
    echo "velox_loadgen not built at $LOADGEN -- build first." >&2
    exit 1
fi
if [[ ! -x "$VIZ" ]]; then
    echo "velox_viz not built at $VIZ -- build first." >&2
    exit 1
fi

OFF_JSON="$RUNS_DIR/viz_off.json"
ON_JSON="$RUNS_DIR/viz_on.json"
BASELINE_SHAPED="$RUNS_DIR/viz_off_as_baseline.json"

echo "=== A: engine path, no visualizer ==="
"$LOADGEN" --path=engine --rate="$RATE" --samples="$SAMPLES" --warmup=200000 \
    --json-out="$OFF_JSON"

# check_regression.py wants a baseline-shaped file: reuse A's own numbers as B's comparison
# point, rather than the committed benchmarks/baselines/summary.json -- this script measures
# "does attaching the visualizer regress THIS run", not "does this run regress vs history".
python3 - "$OFF_JSON" "$BASELINE_SHAPED" << 'PYEOF'
import json, sys
with open(sys.argv[1]) as f:
    off = json.load(f)
baseline = {
    "scenario": off["scenario"],
    "p50_ns": off["p50_ns"],
    "p99_ns": off["p99_ns"],
    "budgets": {"p99_regression_pct": 20},
}
if "p999_ns" in off:
    baseline["p999_ns"] = off["p999_ns"]
with open(sys.argv[2], "w") as f:
    json.dump(baseline, f, indent=2)
PYEOF

echo
echo "=== B: engine path, velox_viz attached + one WebSocket client connected ==="

WS_CLIENT_PY="$(mktemp)"
cat > "$WS_CLIENT_PY" << 'PYEOF'
import base64, socket, sys, time
port = int(sys.argv[1])
s = socket.create_connection(("127.0.0.1", port), timeout=10)
key = base64.b64encode(b"decouplingsmoke1").decode()
req = (f"GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nUpgrade: websocket\r\n"
       f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n")
s.sendall(req.encode())
resp = s.recv(4096)
assert b"101" in resp.split(b"\r\n")[0], resp
s.settimeout(1.0)
deadline = time.time() + 3600
while time.time() < deadline:
    try:
        if not s.recv(65536):
            break
    except socket.timeout:
        continue
PYEOF

"$LOADGEN" --path=engine --rate="$RATE" --samples="$SAMPLES" --warmup=200000 \
    --md-port="$MD_PORT" --stats-port="$STATS_PORT" --json-out="$ON_JSON" &
LOADGEN_PID=$!

sleep 1
"$VIZ" --port="$VIZ_PORT" --md="127.0.0.1:$MD_PORT" --stats="127.0.0.1:$STATS_PORT" \
    > "$RUNS_DIR/viz_decoupling_viz.log" 2>&1 &
VIZ_PID=$!

sleep 1
python3 "$WS_CLIENT_PY" "$VIZ_PORT" &
WS_PID=$!

set +e
wait "$LOADGEN_PID"
LOADGEN_STATUS=$?
set -e

kill "$VIZ_PID" "$WS_PID" 2>/dev/null || true
wait "$VIZ_PID" 2>/dev/null || true
wait "$WS_PID" 2>/dev/null || true
rm -f "$WS_CLIENT_PY"

if [[ $LOADGEN_STATUS -ne 0 ]]; then
    echo "velox_loadgen (run B) exited with status $LOADGEN_STATUS" >&2
    exit 1
fi

echo
echo "=== comparing B (viz attached) against A (no viz) ==="
set +e
python3 "$ROOT_DIR/scripts/check_regression.py" \
    --baseline "$BASELINE_SHAPED" --result "$ON_JSON"
STATUS=$?
set -e

echo
echo "off (A): $(python3 -c "import json;d=json.load(open('$OFF_JSON'));print('p99=%dns rate_sustained=%s pool_exhausted=%s' % (d['p99_ns'], d.get('rate_sustained'), d.get('pool_exhausted')))")"
echo "on  (B): $(python3 -c "import json;d=json.load(open('$ON_JSON'));print('p99=%dns rate_sustained=%s pool_exhausted=%s' % (d['p99_ns'], d.get('rate_sustained'), d.get('pool_exhausted')))")"

exit $STATUS
