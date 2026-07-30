#!/usr/bin/env python3
"""Spec 009 T6: the regression gate.

Compares a velox_loadgen JSON result against the committed baseline
(benchmarks/baselines/summary.json). Per constitution Principle 1, only p99 gates -- p50/p999
movement is reported informationally. A scenario present in the result but absent from the
baseline is a new scenario, not a regression, and warns rather than fails.

Exit codes:
  0 -- within budget (or a new scenario with nothing to compare against)
  1 -- p99 regressed more than the budgeted percentage vs baseline
  2 -- the run itself is untrustworthy (rate not sustained, or pool exhaustion) -- refuse to
       compare an untrustworthy run rather than passing it
"""

import argparse
import json
import sys


def load(path):
    with open(path) as f:
        return json.load(f)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--result", required=True)
    ap.add_argument("--report-only", action="store_true",
                    help="print findings but always exit 0 (advisory mode, e.g. cloud CI)")
    args = ap.parse_args()

    result = load(args.result)

    if not result.get("rate_sustained", True):
        print(f"REFUSING TO COMPARE: rate_sustained=false for scenario "
              f"'{result.get('scenario', '?')}' -- this run did not sustain its target rate, "
              f"so its latency figures are not a measurement of that rate.")
        return 0 if args.report_only else 2

    if result.get("pool_exhausted", False):
        print(f"REFUSING TO COMPARE: pool_exhausted=true for scenario "
              f"'{result.get('scenario', '?')}' -- this run measured the REJECT path, not "
              f"matching.")
        return 0 if args.report_only else 2

    try:
        baseline = load(args.baseline)
    except FileNotFoundError:
        print(f"No baseline at {args.baseline} yet -- nothing to compare against. "
              f"This run is a candidate for /perf-baseline.")
        return 0

    scenario = result.get("scenario", baseline.get("scenario"))
    budget_pct = baseline.get("budgets", {}).get("p99_regression_pct", 20)

    # The baseline JSON is either the flat top-level format (the historical
    # steady_limit_orders baseline) or carries a "scenarios" map (Spec 009 T3/decision 3).
    # Compare against a per-scenario entry if one exists for this scenario name; otherwise fall
    # back to the top-level fields, and if THAT scenario name doesn't match either, this is a
    # new scenario -- warn, don't fail.
    scenarios = baseline.get("scenarios", {})
    if scenario in scenarios:
        base_p99 = scenarios[scenario]["p99_ns"]
        base_p50 = scenarios[scenario].get("p50_ns")
        base_p999 = scenarios[scenario].get("p999_ns")
    elif baseline.get("scenario") == scenario:
        base_p99 = baseline["p99_ns"]
        base_p50 = baseline.get("p50_ns")
        base_p999 = baseline.get("p999_ns")
    else:
        print(f"WARN: scenario '{scenario}' has no baseline entry yet -- a new scenario is not "
              f"a regression. This run is a candidate for /perf-baseline.")
        return 0

    result_p99 = result["p99_ns"]
    pct_change = 100.0 * (result_p99 - base_p99) / base_p99

    print(f"scenario:     {scenario}")
    print(f"p99 baseline: {base_p99} ns")
    print(f"p99 result:   {result_p99} ns  ({pct_change:+.1f}%)")
    if base_p50 is not None and "p50_ns" in result:
        p50_pct = 100.0 * (result["p50_ns"] - base_p50) / base_p50
        print(f"p50 movement: {base_p50} ns -> {result['p50_ns']} ns  ({p50_pct:+.1f}%)  "
              f"[informational only]")
    if base_p999 is not None and "p999_ns" in result:
        p999_pct = 100.0 * (result["p999_ns"] - base_p999) / base_p999
        print(f"p999 movement: {base_p999} ns -> {result['p999_ns']} ns  ({p999_pct:+.1f}%)  "
              f"[informational only]")

    if pct_change > budget_pct:
        msg = (f"P99 REGRESSION: {scenario} regressed {pct_change:.1f}% "
              f"(budget is {budget_pct}%) -- {base_p99} ns -> {result_p99} ns")
        print(msg)
        return 0 if args.report_only else 1

    print(f"OK: p99 within budget ({pct_change:+.1f}% <= {budget_pct}%)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
