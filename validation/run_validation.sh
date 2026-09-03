#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
REALTEST="${REALTEST_CSV_PATH:-$ROOT/storage/backtests/final_tests/pureRSI.csv}"
WARMUP_DAYS="${HISTORICAL_WARMUP_DAYS:-30}"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
BASELINE_CHECKER="$ROOT/tools/distributed_compare/check_realtest_baseline.py"

# A failed full replay is expensive. Preserve its topology by default so the
# existing NATS/PostgreSQL state can be inspected instead of rerunning blindly.
export HISTORICAL_KEEP_ON_FAILURE="${HISTORICAL_KEEP_ON_FAILURE:-1}"

# Keep the exact research comparison policy but do not stop for ENTER on the
# already-known BNB/FET/ZEC mismatches. Normal research runs remain interactive.
export REALTEST_NONINTERACTIVE=1

fail() {
    echo "[FAIL] $*" >&2
    exit 1
}

[[ -f "$DATA" ]] || fail "historical source not found: $DATA"
[[ -f "$REALTEST" ]] || fail "RealTest CSV not found: $REALTEST"
[[ -x "$HARNESS" ]] || fail "historical replay harness missing: $HARNESS"
[[ -x "$BASELINE_CHECKER" ]] || fail "RealTest baseline checker missing: $BASELINE_CHECKER"
[[ "$WARMUP_DAYS" =~ ^[0-9]+$ ]] || fail "HISTORICAL_WARMUP_DAYS must be a non-negative integer"

range_output="$({ python3 - "$DATA" "$WARMUP_DAYS" <<'PYRANGE'
import csv
import sys
from datetime import datetime, timedelta
from pathlib import Path

path = Path(sys.argv[1]).resolve()
warmup = int(sys.argv[2])

def parse(raw: str):
    return datetime.strptime(raw, "%Y-%m-%d").date()

with path.open(newline="", encoding="utf-8") as handle:
    reader = csv.DictReader(handle)
    if not reader.fieldnames or "date" not in reader.fieldnames:
        raise SystemExit("FULL_HISTORY_ERROR=CSV must contain a 'date' column")
    dates = {
        parse((row.get("date") or "").strip())
        for row in reader
        if (row.get("date") or "").strip()
    }

if not dates:
    raise SystemExit("FULL_HISTORY_ERROR=historical source contains no dates")

first = min(dates)
last = max(dates)
current = first
missing = []
while current <= last:
    if current not in dates:
        missing.append(current.isoformat())
        if len(missing) >= 8:
            break
    current += timedelta(days=1)
if missing:
    suffix = " ..." if len(missing) >= 8 else ""
    raise SystemExit(
        "FULL_HISTORY_ERROR=missing whole-market dates: " + ", ".join(missing) + suffix
    )

measure_start = first + timedelta(days=warmup)
measure_end = last - timedelta(days=1)
if measure_start > measure_end:
    raise SystemExit("FULL_HISTORY_ERROR=source is too short for warmup plus final T+1 open")

measure_days = (measure_end - measure_start).days + 1
expected_cycles = (measure_end - first).days + 1

print(f"FULL_SOURCE_FIRST={first.isoformat()}")
print(f"FULL_SOURCE_LAST={last.isoformat()}")
print(f"FULL_MEASURE_START={measure_start.isoformat()}")
print(f"FULL_MEASURE_END={measure_end.isoformat()}")
print(f"FULL_MEASURE_DAYS={measure_days}")
print(f"FULL_EXPECTED_CYCLES={expected_cycles}")
print(f"FULL_FINAL_OPEN={last.isoformat()}")
PYRANGE
} 2>&1)" || {
    echo "[FAIL] unable to resolve full historical range"
    echo "$range_output"
    exit 1
}
eval "$range_output"

latest_comparator_log() {
    local dir="$1"
    find "$dir" -mindepth 2 -maxdepth 2 -type f -name '05_distributed_fast_compare.log' \
        -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -n1 | cut -d' ' -f2-
}

run_mode() {
    local label="$1"
    local mode="$2"
    local nats_port="$3"
    local monitor_port="$4"
    local postgres_port="$5"
    local project="$6"
    local log_root="$7"
    local comparison_csv="$8"
    local checker_mode="$9"

    echo
    echo "============================================================"
    echo "VALIDATION — $label"
    echo "============================================================"

    bash "$HARNESS" \
        --historical-data "$DATA" \
        --start-date "$FULL_MEASURE_START" \
        --days "$FULL_MEASURE_DAYS" \
        --warmup-days "$WARMUP_DAYS" \
        --portfolio-mode "$mode" \
        --realtest-csv "$REALTEST" \
        --realtest-comparison-csv "$comparison_csv" \
        --require-trading \
        --nats-port "$nats_port" \
        --nats-monitor-port "$monitor_port" \
        --postgres-port "$postgres_port" \
        --project "$project" \
        --log-root "$log_root"

    local comparator_log
    comparator_log="$(latest_comparator_log "$log_root")"
    [[ -n "$comparator_log" && -f "$comparator_log" ]] || \
        fail "$label comparator log not found"

    python3 "$BASELINE_CHECKER" --mode "$checker_mode" --log "$comparator_log"
    echo "[PASS] $label exact RealTest known baseline unchanged"
}

printf '%s\n' "============================================================"
printf '%s\n' "ALGO TRADING — COMPLETE ECONOMIC REGRESSION"
printf 'ROOT       : %s\n' "$ROOT"
printf 'DATA       : %s\n' "$DATA"
printf 'REALTEST   : %s\n' "$REALTEST"
printf 'SOURCE     : %s .. %s\n' "$FULL_SOURCE_FIRST" "$FULL_SOURCE_LAST"
printf 'CYCLES     : %s per portfolio mode\n' "$FULL_EXPECTED_CYCLES"
printf 'MEASURE    : %s .. %s\n' "$FULL_MEASURE_START" "$FULL_MEASURE_END"
printf 'FINAL OPEN : %s\n' "$FULL_FINAL_OPEN"
printf '%s\n' "============================================================"
echo "[INFO] One command validates full-history EqualWeight + VolTarget."
echo "[INFO] Each mode must satisfy distributed == fast before exact research RealTest baseline checking."
echo "[INFO] RealTest comparison logic is unchanged; only mismatch pauses are disabled for this automation."

mkdir -p "$SCRIPT_DIR/logs/equal_weight" "$SCRIPT_DIR/logs/vol_target"

run_mode \
    "PURE RSI + EQUAL WEIGHT" \
    "equal-weight" \
    54241 58241 55451 \
    "algotrading_validation_equal_weight" \
    "$SCRIPT_DIR/logs/equal_weight" \
    "$ROOT/storage/backtests/distributed_equal_weight_realtest_comparison.csv" \
    "equal-weight"

run_mode \
    "PURE RSI + VOLATILITY TARGET" \
    "vol-target" \
    54242 58242 55452 \
    "algotrading_validation_vol_target" \
    "$SCRIPT_DIR/logs/vol_target" \
    "$ROOT/storage/backtests/distributed_vol_target_realtest_campaign_comparison.csv" \
    "vol-target"

echo
echo "============================================================"
echo "COMPLETE VALIDATION RESULT: PASS"
echo "============================================================"
echo "[PASS] EqualWeight full distributed == fast"
echo "[PASS] EqualWeight exact research RealTest accepted baseline"
echo "[PASS] VolTarget full distributed == fast"
echo "[PASS] VolTarget exact research RealTest accepted baseline"
echo "[PASS] PostgreSQL/service health gates passed in both distributed runs"
echo "============================================================"
