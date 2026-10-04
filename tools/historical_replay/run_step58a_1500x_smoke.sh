#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
RUN="$ROOT/deploy/historical_replay/run/step58a_1500x"
BIN="$ROOT/deploy/historical_replay/run/step58a/bin/step58a_full_system_replay_runner"
mkdir -p "$RUN"
rm -rf "$RUN/mock-state"

bash "$ROOT/tools/historical_replay/build_step58a_runner.sh" "$ROOT" "$BIN"

printf '%s\n' 'STEP58A 1500x smoke: warm-up is unpaced; 2020-04-15 itself is released through TimeHandler at true 1500x.'
printf '%s\n' 'Expected paced wait is about 57.6 seconds for one 24-hour business day.'

"$BIN" \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$RUN/mock-state" \
  --output "$RUN/summary.json" \
  --fills-output "$RUN/fills.csv" \
  --start 2020-01-01 --end 2020-04-15 \
  --profile mock-default \
  --pace-start 2020-04-15 --pace-end 2020-04-15 \
  --speed 1500

python3 -S - "$RUN/summary.json" <<'PY'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert s['result']=='PASS'
assert s['releaseMode']=='TIMEHANDLER_PACED_SEGMENT'
assert s['paceStart']=='2020-04-15' and s['paceEnd']=='2020-04-15'
assert s['timeHandlerSpeed']==1500
assert s['profile']=='mock-default'
assert s['reconciliation']=='CLEAN' and s['reconciliationIssues']==0
assert s['routeSafe'] is True
assert s['canonicalFills']>0
assert s['canonicalAccountingEvents']==s['canonicalFills']
PY

python3 -S "$ROOT/tools/historical_replay/check_step58a_fill_structure.py" \
  --candidate "$RUN/fills.csv" \
  --reference "$ROOT/deploy/historical_replay/run/t19_full_history_realtest_fills.csv" \
  --end-date 20200415 \
  --summary "$RUN/structure_summary.json"

printf '\n%s\n' '============================================================'
printf '%s\n' 'STEP 58A-1500X: PASS — TRUE TIMEHANDLER PACED SMOKE VALIDATED'
printf '%s\n' '============================================================'
printf 'summary=%s\n' "$RUN/summary.json"
printf 'fills=%s\n' "$RUN/fills.csv"
