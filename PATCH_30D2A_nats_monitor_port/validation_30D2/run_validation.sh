#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
WARMUP_DAYS="${HISTORICAL_WARMUP_DAYS:-30}"
DIVERGENCE_DATE="${HISTORICAL_DIVERGENCE_DATE:-2021-04-12}"

printf '%s\n' "============================================================"
printf '%s\n' "PATCH 30D2 VALIDATION — EXACT HOLD QUANTITY"
printf 'ROOT       : %s\n' "$ROOT"
printf 'DATA       : %s\n' "$DATA"
printf 'CHECK THRU : %s\n' "$DIVERGENCE_DATE"
printf '%s\n' "============================================================"

[[ -f "$DATA" ]] || { echo "[FAIL] historical source not found: $DATA"; exit 1; }
[[ -x "$ROOT/tools/distributed_compare/run_historical_compare.sh" ]] || {
    echo "[FAIL] historical replay harness missing"
    exit 1
}
[[ "$WARMUP_DAYS" =~ ^[0-9]+$ ]] || { echo "[FAIL] HISTORICAL_WARMUP_DAYS must be a non-negative integer"; exit 1; }

window_output="$({ python3 - "$DATA" "$WARMUP_DAYS" "$DIVERGENCE_DATE" <<'PY'
import csv
import sys
from datetime import datetime, timedelta
from pathlib import Path

path = Path(sys.argv[1]).resolve()
warmup = int(sys.argv[2])
divergence = datetime.strptime(sys.argv[3], "%Y-%m-%d").date()

with path.open(newline="", encoding="utf-8") as handle:
    reader = csv.DictReader(handle)
    if not reader.fieldnames or "date" not in reader.fieldnames:
        raise SystemExit("RANGE_ERROR=CSV must contain a date column")
    dates = {
        datetime.strptime((row.get("date") or "").strip(), "%Y-%m-%d").date()
        for row in reader
        if (row.get("date") or "").strip()
    }

if not dates:
    raise SystemExit("RANGE_ERROR=historical source contains no dates")

first = min(dates)
last = max(dates)
measure_start = first + timedelta(days=warmup)
final_open = divergence + timedelta(days=1)

if divergence < measure_start:
    raise SystemExit("RANGE_ERROR=divergence date is inside warmup")
if final_open > last:
    raise SystemExit("RANGE_ERROR=source does not contain final T+1 open")

current = first
while current <= final_open:
    if current not in dates:
        raise SystemExit(f"RANGE_ERROR=missing whole-market date {current.isoformat()}")
    current += timedelta(days=1)

measure_days = (divergence - measure_start).days + 1
expected_cycles = (divergence - first).days + 1

print(f"MEASURE_START={measure_start.isoformat()}")
print(f"MEASURE_DAYS={measure_days}")
print(f"EXPECTED_CYCLES={expected_cycles}")
print(f"FINAL_OPEN={final_open.isoformat()}")
PY
} 2>&1)" || {
    echo "[FAIL] unable to resolve diagnostic historical range"
    echo "$window_output"
    exit 1
}
eval "$window_output"

printf '[INFO] replaying from source start through %s: cycles=%s measured_days=%s final_open=%s\n' \
    "$DIVERGENCE_DATE" "$EXPECTED_CYCLES" "$MEASURE_DAYS" "$FINAL_OPEN"

# This patch changes lib/runtime code, so force a fresh runtime image. Reusing the
# old step28 image would silently run the pre-30D2 OrderPlanner inside Docker.
bash "$ROOT/tools/distributed_compare/run_historical_compare.sh" \
    --historical-data "$DATA" \
    --start-date "$MEASURE_START" \
    --days "$MEASURE_DAYS" \
    --warmup-days "$WARMUP_DAYS" \
    --require-trading \
    --force-runtime-build \
    --nats-port 54234 \
    --nats-monitor-port 58234 \
    --postgres-port 55444 \
    --project algotrading_step30d2_validation \
    --log-root "$SCRIPT_DIR/logs"

echo
echo "============================================================"
echo "PATCH 30D2 RESULT: PASS"
echo "Historical equivalence is clean through $DIVERGENCE_DATE."
echo "Next: rerun validation_30D full history."
echo "============================================================"
