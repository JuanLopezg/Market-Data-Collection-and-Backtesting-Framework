#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
RUN="$ROOT/deploy/historical_replay/run/step58a_full_history_realtest"
BIN="$ROOT/deploy/historical_replay/run/step58a/bin/step58a_full_system_replay_runner"
REALTEST="$ROOT/storage/backtests/final_tests/pureRSI.csv"
HISTORICAL="$ROOT/storage/databases/1d_cmc.csv"
BASELINE="$ROOT/tools/historical_replay/realtest_known_baseline_snapshot.json"
REFERENCE_FILLS="$ROOT/deploy/historical_replay/run/t19_full_history_realtest_fills.csv"
T19_BIN="$ROOT/deploy/historical_replay/run/t19_realtest_compare"
mkdir -p "$RUN"
rm -rf "$RUN/mock-state"

for f in "$REALTEST" "$HISTORICAL" "$BASELINE" "$REFERENCE_FILLS"; do
  [[ -f "$f" ]] || { echo "STEP58A_FULL_FAIL: missing required evidence $f" >&2; exit 1; }
done
[[ -f "$ROOT/build/lib/src/libalgolib.so" ]] || {
  echo 'STEP58A_FULL_FAIL: build/lib/src/libalgolib.so missing; build the project library before this campaign.' >&2
  exit 1
}

bash "$ROOT/tools/historical_replay/build_step58a_runner.sh" "$ROOT" "$BIN"

# Comparison profile: same production Strategy/Risk/Planner and canonical MOCK path,
# but removes synthetic matcher slippage/liquidity caps from the historical-parity question.
# Business/event timestamps stay ordered; this full-history run is intentionally unpaced.
"$BIN" \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$RUN/mock-state" \
  --output "$RUN/runtime_summary.json" \
  --fills-output "$RUN/fills.csv" \
  --start 2020-01-01 --end 2025-10-13 \
  --profile realtest-parity \
  --speed 1500

python3 -S - "$RUN/runtime_summary.json" <<'PY'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert s['result']=='PASS'
assert s['profile']=='realtest-parity'
assert s['releaseMode']=='UNPACED_BUSINESS_TIME'
assert s['firstDate']=='2020-01-01' and s['lastDate']=='2025-10-13'
assert s['volumeTransform']=='v_prime=0.1*v+10000000000'
assert s['matcherParticipationPpm']==1000000
assert s['matcherMaxAdverseSlippagePpm']==0
assert s['reconciliation']=='CLEAN' and s['reconciliationIssues']==0
assert s['routeSafe'] is True
assert s['canonicalFills']>0
assert s['canonicalAccountingEvents']==s['canonicalFills']
assert s['feesPaid']>0
PY

python3 -S "$ROOT/tools/historical_replay/check_step58a_fill_structure.py" \
  --candidate "$RUN/fills.csv" \
  --reference "$REFERENCE_FILLS" \
  --summary "$RUN/structure_summary.json"

# Compile and invoke the exact research/src/realtest.cpp bridge used by T19.
python3 -S "$ROOT/tools/historical_replay/run_t19_realtest_gate.py" --root "$ROOT" --compile-only

export REALTEST_NONINTERACTIVE=1
"$T19_BIN" \
  --fills-csv "$RUN/fills.csv" \
  --realtest-csv "$REALTEST" \
  --historical-data "$HISTORICAL" \
  --comparison-csv "$RUN/realtest_comparison.csv" \
  --portfolio-mode equal-weight > "$RUN/realtest.log" 2>&1

python3 -S "$ROOT/tools/historical_replay/check_step58a_realtest_policy.py" \
  --log "$RUN/realtest.log" \
  --baseline "$BASELINE" \
  --summary "$RUN/realtest_policy_summary.json"

python3 -S - "$RUN/runtime_summary.json" "$RUN/structure_summary.json" "$RUN/realtest_policy_summary.json" "$RUN/acceptance_summary.json" <<'PY'
import hashlib,json,pathlib,sys
runtime=json.loads(pathlib.Path(sys.argv[1]).read_text())
structure=json.loads(pathlib.Path(sys.argv[2]).read_text())
policy=json.loads(pathlib.Path(sys.argv[3]).read_text())
payload={
  'result':'PASS',
  'scope':'STEP58A_FULL_HISTORY_REALTEST_EQUIVALENCE',
  'runtime':runtime,
  'fillStructure':structure,
  'realtestPolicy':policy,
  'realtestNetPnlEqualityRequired':False,
  'reason':'MOCK accounting includes canonical 400ppm fees while the historical RealTest fill bridge compares gross structural behavior with commission=0',
}
canon=json.dumps(payload,sort_keys=True,separators=(',',':')).encode()
payload['acceptanceFingerprint']=hashlib.sha256(canon).hexdigest()
pathlib.Path(sys.argv[4]).write_text(json.dumps(payload,indent=2,sort_keys=True)+'\n')
PY

printf '\n%s\n' '============================================================'
printf '%s\n' 'STEP 58A-FULL: PASS — FULL HISTORY + REALTEST EQUIVALENCE VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Historical operation identity and the exact T19 RealTest accepted-exception policy are unchanged.'
printf '%s\n' 'MOCK 400ppm fee accounting is validated separately; net PnL is intentionally not asserted equal to RealTest.'
printf 'summary=%s\n' "$RUN/acceptance_summary.json"
