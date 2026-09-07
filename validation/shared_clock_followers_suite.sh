#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
LOG_ROOT="$SCRIPT_DIR/logs/clock_followers"
PROJECT="${CLOCK_FOLLOWER_PROJECT:-algotrading_validation_clock_followers}"

python3 "$SCRIPT_DIR/shared_clock_followers_check.py" --root "$ROOT"

echo "============================================================"
echo "STEP 35C — INCREMENTAL BUILD"
echo "============================================================"
ninja -C "$BUILD" -j8
echo "[PASS] full project build after clock follower wiring"

# Re-run the original time-source audit after the wiring. It must remain free of direct
# business wall-clock findings and should now report all 8 runtime clock participants.
audit_output="$(bash "$SCRIPT_DIR/clock_audit.sh")"
printf '%s\n' "$audit_output"
grep -Fq "blocking business findings   : 0" <<<"$audit_output"
grep -Fq "runtime Clock consumers      : 8/8" <<<"$audit_output"
grep -Fq "STEP 35A CLOCK AUDIT RESULT: PASS" <<<"$audit_output"
echo "[PASS] clock audit remains clean and runtime consumers are 8/8"

mkdir -p "$LOG_ROOT"

echo "============================================================"
echo "STEP 35C — SHORT DISTRIBUTED CAUSAL-CLOCK REPLAY"
echo "============================================================"

bash "$ROOT/tools/distributed_compare/run_historical_compare.sh" \
  --start-date 2021-03-01 \
  --days 10 \
  --warmup-days 30 \
  --portfolio-mode equal-weight \
  --project "$PROJECT" \
  --nats-port 54243 \
  --nats-monitor-port 58243 \
  --postgres-port 55453 \
  --log-root "$LOG_ROOT" \
  --force-runtime-build

latest="$(find "$LOG_ROOT" -mindepth 1 -maxdepth 1 -type d -printf '%T@ %p\n' | sort -nr | head -1 | cut -d' ' -f2-)"
[[ -n "$latest" && -d "$latest" ]] || { echo "[FAIL] could not locate STEP 35C replay logs" >&2; exit 1; }

followers=(market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange)
for service in "${followers[@]}"; do
  log="$latest/services/$service.log"
  [[ -f "$log" ]] || { echo "[FAIL] missing service log: $log" >&2; exit 1; }
  if ! grep -Fq "event=clock_synchronized" "$log"; then
    echo "[FAIL] $service never proved REPLAY clock synchronization" >&2
    tail -80 "$log" >&2 || true
    exit 1
  fi
  echo "[PASS] $service synchronized to authoritative replay clock"
done

controller_log="$latest/services/replay-controller.log"
[[ -f "$controller_log" ]] || { echo "[FAIL] missing replay-controller log" >&2; exit 1; }
grep -Fq "event=clock_state_published" "$controller_log"

echo "[PASS] replay-controller published authoritative ClockState"

echo "============================================================"
echo "STEP 35C CLOCK FOLLOWERS RESULT: PASS"
echo "Validated: 7 REPLAY followers + authority, causal produced_at gating,"
echo "           LIVE-safe bootstrap split, and short distributed == fast replay."
echo "Next: STEP 35D — hard-restart follower re-sync + clock causality recovery proof"
echo "============================================================"
