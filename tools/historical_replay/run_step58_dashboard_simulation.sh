#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
STATE_DIR="${STEP58_STATE_DIR:-$ROOT/deploy/historical_replay/run/step58_dashboard}"
BIN="$STATE_DIR/bin/step58_dashboard_simulation_runner"
CSV="${STEP58_CSV:-$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv}"
MAPPING="${STEP58_MAPPING:-$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv}"
START="${STEP58_START_DATE:-2020-01-01}"
END="${STEP58_END_DATE:-2020-04-16}"
SPEED="${STEP58_SPEED:-100000000}"
UI_DELAY_MS="${STEP58_UI_DELAY_MS:-350}"

mkdir -p "$STATE_DIR/bin" "$STATE_DIR/requests" "$STATE_DIR/processed"
if [[ ! -x "$BIN" || "${STEP58_REBUILD:-0}" == "1" ]]; then
  bash "$ROOT/tools/historical_replay/build_step58_dashboard_simulation_runner.sh" "$ROOT" "$BIN"
fi
rm -f "$STATE_DIR/state.json" "$STATE_DIR/state.json.tmp"
rm -rf "$STATE_DIR/venue_durable"
mkdir -p "$STATE_DIR/venue_durable"

printf '%s\n' 'STEP58 dashboard simulation starting.'
printf 'window=%s..%s uiDelayMs=%s state=%s\n' "$START" "$END" "$UI_DELAY_MS" "$STATE_DIR/state.json"
printf '%s\n' 'Keep this terminal open. After replay completes it stays in MANUAL_READY for Step57-backed manual MOCK requests.'

exec "$BIN" \
  --csv "$CSV" \
  --mapping "$MAPPING" \
  --durable "$STATE_DIR/venue_durable" \
  --state-dir "$STATE_DIR" \
  --start "$START" --end "$END" \
  --speed "$SPEED" --ui-delay-ms "$UI_DELAY_MS"
