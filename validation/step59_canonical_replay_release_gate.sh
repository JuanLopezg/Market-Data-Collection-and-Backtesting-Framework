#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
TMP="$(mktemp -d)"
if [[ "${STEP59_KEEP_TMP:-0}" == "1" ]]; then
  trap 'printf "STEP59: kept temp evidence at %s\n" "$TMP"' EXIT
else
  trap 'rm -rf "$TMP"' EXIT
fi

START="2020-01-01"
END="2020-04-16"
EXPECTED_DAYS=107
EXPECTED_FILLS=50
EXPECTED_TRADES=25
EXPECTED_FP="94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2"

pass(){ printf 'STEP59-RELEASE: PASS: %s\n' "$*"; }
fail(){ printf 'STEP59-RELEASE: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 59 — CANONICAL REPLAY / DASHBOARD RELEASE GATE'
printf '%s\n' '============================================================'
printf '%s\n' 'Compact 107-day proof: RealTest parity, determinism, restart/resume, dashboard equality and paced-speed invariance.'
printf '%s\n' 'No Docker/UI and no full-history replay are required.'
printf '\n'

for f in \
  "$ROOT/research/replay.py" \
  "$ROOT/research/src/canonical/canonical_replay.cpp" \
  "$ROOT/storage/backtests/final_tests/pureRSI.csv" \
  "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done

printf '%s\n' '[1/7] Build the active canonical runner once and verify the public restart/dashboard CLI'
python3 -m py_compile "$ROOT/research/replay.py" || fail 'research/replay.py syntax failed'
python3 "$ROOT/research/replay.py" system --help >"$TMP/system-help.txt"
python3 "$ROOT/research/replay.py" dashboard --help >"$TMP/dashboard-help.txt"
for flag in '--checkpoint-every' '--stop-after-days' '--resume'; do
  grep -Fq -- "$flag" "$TMP/system-help.txt" || fail "system CLI missing $flag"
  grep -Fq -- "$flag" "$TMP/dashboard-help.txt" || fail "dashboard CLI missing $flag"
done
grep -Fq -- '--no-dashboard-up' "$TMP/dashboard-help.txt" || fail 'dashboard CLI missing --no-dashboard-up'
RUNNER="$(python3 - "$ROOT" <<'PY'
import importlib.util,pathlib,sys
root=pathlib.Path(sys.argv[1]); path=root/'research/replay.py'
spec=importlib.util.spec_from_file_location('replay',path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
print(mod.build_full_runner(True))
PY
)"
RUNNER="${RUNNER##*$'\n'}"
[[ -x "$RUNNER" ]] || fail 'canonical runner did not compile' 
pass 'active canonical runner compiles and system/dashboard expose checkpoint/resume controls'

run_case(){
  local name="$1" mode="$2" speed="$3"
  shift 3
  local dir="$TMP/$name"
  mkdir -p "$dir"
  local cmd=(
    "$RUNNER"
    --mode "$mode"
    --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv"
    --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv"
    --durable "$dir/mock_state"
    --output "$dir/runtime_summary.json"
    --fills-output "$dir/fills.csv"
    --start "$START"
    --end "$END"
    --profile realtest-parity
    --speed "$speed"
    --checkpoint-state "$dir/replay_checkpoint_v1.txt"
  )
  if [[ "$mode" == "dashboard" ]]; then
    mkdir -p "$dir/dashboard_state"
    cmd+=(
      --dashboard-state-dir "$dir/dashboard_state"
      --visual-start "$START"
      --visual-end "$END"
      --ui-delay-ms 0
    )
  fi
  cmd+=("$@")
  "${cmd[@]}" >"$dir/runtime.log" 2>&1 || {
    cat "$dir/runtime.log" >&2
    fail "$name runtime failed"
  }
}

check_pass_summary(){
  local path="$1" expected_fp="$2"
  python3 - "$path" "$EXPECTED_DAYS" "$EXPECTED_FILLS" "$expected_fp" <<'PY'
import json,pathlib,sys
p=pathlib.Path(sys.argv[1]); days=int(sys.argv[2]); fills=int(sys.argv[3]); fp=sys.argv[4]
s=json.loads(p.read_text())
assert s['result']=='PASS', s
assert s['days']==days, (s.get('days'),days)
assert s['canonicalFills']==fills, (s.get('canonicalFills'),fills)
assert s['fullRunFingerprint']==fp, (s.get('fullRunFingerprint'),fp)
assert s['reconciliation']=='CLEAN', s.get('reconciliation')
assert s['routeSafe'] is True
assert s['feesPaid']==0
assert s['matcherIgnoresVolumeCapacity'] is True
PY
}

printf '%s\n' '[2/7] 107-day RealTest parity baseline is exact and exception-free'
run_case system_a system 1500
check_pass_summary "$TMP/system_a/runtime_summary.json" "$EXPECTED_FP" || fail 'system baseline summary mismatch'
REALTEST_NONINTERACTIVE=1 python3 - "$ROOT" "$TMP/system_a/fills.csv" "$TMP/system_a/realtest" "$EXPECTED_TRADES" <<'PY'
import importlib.util,pathlib,sys
root=pathlib.Path(sys.argv[1]); fills=pathlib.Path(sys.argv[2]); out=pathlib.Path(sys.argv[3]); expected=int(sys.argv[4])
path=root/'research/replay.py'
spec=importlib.util.spec_from_file_location('replay',path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
trades=mod.trades_from_fill_csv(fills,'2020-04-16')
r=mod.compare_candidate_trades_to_realtest(
    trades,'2020-01-01','2020-04-16',out,manual_review=False,
)
assert r['result']=='PASS'
assert r['referenceTradesInWindow']==expected
assert r['candidateTradesInWindow']==expected
assert r['fullyMatched']==expected
assert r['differences']==0
assert r['knownExceptionsInCode']==0
PY
pass '107-day realtest-parity remains 25/25, 0 differences, 50 fills and the accepted fingerprint'

printf '%s\n' '[3/7] Repeated continuous system run is deterministic'
run_case system_b system 1500
check_pass_summary "$TMP/system_b/runtime_summary.json" "$EXPECTED_FP" || fail 'second system summary mismatch'
cmp "$TMP/system_a/fills.csv" "$TMP/system_b/fills.csv" >/dev/null || fail 'repeated system fills are not byte-identical'
pass 'two clean system runs produce the same fingerprint and byte-identical fills'

printf '%s\n' '[4/7] System checkpoint at day 50 resumes to the continuous baseline'
mkdir -p "$TMP/system_restart"
"$RUNNER" \
  --mode system \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$TMP/system_restart/mock_state" \
  --output "$TMP/system_restart/runtime_summary.json" \
  --fills-output "$TMP/system_restart/fills.csv" \
  --start "$START" --end "$END" \
  --profile realtest-parity --speed 1500 \
  --checkpoint-state "$TMP/system_restart/replay_checkpoint_v1.txt" \
  --checkpoint-every 25 --stop-after-days 50 \
  >"$TMP/system_restart/stop.log" 2>&1 || {
    cat "$TMP/system_restart/stop.log" >&2
    fail 'system checkpoint-stop failed'
  }
python3 - "$TMP/system_restart/runtime_summary.json" <<'PY'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert s['result']=='CHECKPOINTED'
assert s['daysProcessed']==50
assert s['checkpointDate']=='2020-02-19'
PY
"$RUNNER" \
  --mode system \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$TMP/system_restart/mock_state" \
  --output "$TMP/system_restart/runtime_summary.json" \
  --fills-output "$TMP/system_restart/fills.csv" \
  --start "$START" --end "$END" \
  --profile realtest-parity --speed 1500 \
  --checkpoint-state "$TMP/system_restart/replay_checkpoint_v1.txt" \
  --resume --checkpoint-every 25 \
  >"$TMP/system_restart/resume.log" 2>&1 || {
    cat "$TMP/system_restart/resume.log" >&2
    fail 'system resume failed'
  }
check_pass_summary "$TMP/system_restart/runtime_summary.json" "$EXPECTED_FP" || fail 'resumed system summary mismatch'
cmp "$TMP/system_a/fills.csv" "$TMP/system_restart/fills.csv" >/dev/null || fail 'resumed system fills differ from continuous baseline'
pass 'system restart/resume is economically and byte-for-byte identical to a continuous run'

printf '%s\n' '[5/7] Dashboard mode is economically identical to system and also restart-safe'
run_case dashboard_a dashboard 1500
check_pass_summary "$TMP/dashboard_a/runtime_summary.json" "$EXPECTED_FP" || fail 'dashboard baseline summary mismatch'
cmp "$TMP/system_a/fills.csv" "$TMP/dashboard_a/fills.csv" >/dev/null || fail 'dashboard fills differ from system fills'

mkdir -p "$TMP/dashboard_restart/dashboard_state"
"$RUNNER" \
  --mode dashboard \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$TMP/dashboard_restart/mock_state" \
  --output "$TMP/dashboard_restart/runtime_summary.json" \
  --fills-output "$TMP/dashboard_restart/fills.csv" \
  --start "$START" --end "$END" \
  --profile realtest-parity --speed 1500 \
  --checkpoint-state "$TMP/dashboard_restart/replay_checkpoint_v1.txt" \
  --checkpoint-every 25 --stop-after-days 50 \
  --dashboard-state-dir "$TMP/dashboard_restart/dashboard_state" \
  --visual-start "$START" --visual-end "$END" --ui-delay-ms 0 \
  >"$TMP/dashboard_restart/stop.log" 2>&1 || {
    cat "$TMP/dashboard_restart/stop.log" >&2
    fail 'dashboard checkpoint-stop failed'
  }
python3 - "$TMP/dashboard_restart/runtime_summary.json" <<'PY'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert s['result']=='CHECKPOINTED'
assert s['daysProcessed']==50
assert s['checkpointDate']=='2020-02-19'
PY
"$RUNNER" \
  --mode dashboard \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$TMP/dashboard_restart/mock_state" \
  --output "$TMP/dashboard_restart/runtime_summary.json" \
  --fills-output "$TMP/dashboard_restart/fills.csv" \
  --start "$START" --end "$END" \
  --profile realtest-parity --speed 1500 \
  --checkpoint-state "$TMP/dashboard_restart/replay_checkpoint_v1.txt" \
  --resume --checkpoint-every 25 \
  --dashboard-state-dir "$TMP/dashboard_restart/dashboard_state" \
  --visual-start "$START" --visual-end "$END" --ui-delay-ms 0 \
  >"$TMP/dashboard_restart/resume.log" 2>&1 || {
    cat "$TMP/dashboard_restart/resume.log" >&2
    fail 'dashboard resume failed'
  }
check_pass_summary "$TMP/dashboard_restart/runtime_summary.json" "$EXPECTED_FP" || fail 'resumed dashboard summary mismatch'
cmp "$TMP/system_a/fills.csv" "$TMP/dashboard_restart/fills.csv" >/dev/null || fail 'resumed dashboard fills differ from system baseline'
pass 'dashboard == system and dashboard restart/resume returns to the exact same economics'

printf '%s\n' '[6/7] Pacing speed changes time only, not economics'
PACE_DATE="$(python3 - "$TMP/system_a/fills.csv" <<'PY'
import csv,datetime,pathlib,sys
p=pathlib.Path(sys.argv[1])
with p.open(newline='',encoding='utf-8') as fh:
    rows=list(csv.DictReader(fh))
assert rows, 'baseline has no fills'
ts=''.join(ch for ch in rows[0]['timestamp'] if ch.isdigit())[:8]
d=datetime.datetime.strptime(ts,'%Y%m%d').date()
print(d.isoformat())
PY
)"
[[ -n "$PACE_DATE" ]] || fail 'could not derive an active fill date for pacing test'

run_dashboard_paced(){
  local name="$1" speed="$2"
  local dir="$TMP/$name"
  mkdir -p "$dir/dashboard_state"
  "$RUNNER" \
    --mode dashboard \
    --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
    --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
    --durable "$dir/mock_state" \
    --output "$dir/runtime_summary.json" \
    --fills-output "$dir/fills.csv" \
    --start "$START" --end "$END" \
    --profile realtest-parity --speed "$speed" \
    --checkpoint-state "$dir/replay_checkpoint_v1.txt" \
    --pace-start "$PACE_DATE" --pace-end "$PACE_DATE" \
    --dashboard-state-dir "$dir/dashboard_state" \
    --visual-start "$START" --visual-end "$END" --ui-delay-ms 0 \
    >"$dir/runtime.log" 2>&1 || {
      cat "$dir/runtime.log" >&2
      fail "$name paced dashboard failed"
    }
}
run_dashboard_paced dashboard_pace_5000 5000
run_dashboard_paced dashboard_pace_50000 50000
check_pass_summary "$TMP/dashboard_pace_5000/runtime_summary.json" "$EXPECTED_FP" || fail '5000x paced summary mismatch'
check_pass_summary "$TMP/dashboard_pace_50000/runtime_summary.json" "$EXPECTED_FP" || fail '50000x paced summary mismatch'
cmp "$TMP/system_a/fills.csv" "$TMP/dashboard_pace_5000/fills.csv" >/dev/null || fail '5000x pacing changed fills'
cmp "$TMP/dashboard_pace_5000/fills.csv" "$TMP/dashboard_pace_50000/fills.csv" >/dev/null || fail 'paced speeds produced different fills'
pass "dashboard pacing on active date $PACE_DATE is speed-invariant and preserves the system baseline"

printf '%s\n' '[7/7] Release evidence is internally consistent'
python3 - \
  "$TMP/system_a/runtime_summary.json" \
  "$TMP/system_b/runtime_summary.json" \
  "$TMP/system_restart/runtime_summary.json" \
  "$TMP/dashboard_a/runtime_summary.json" \
  "$TMP/dashboard_restart/runtime_summary.json" \
  "$TMP/dashboard_pace_5000/runtime_summary.json" \
  "$TMP/dashboard_pace_50000/runtime_summary.json" <<'PY'
import json,pathlib,sys
summaries=[json.loads(pathlib.Path(p).read_text()) for p in sys.argv[1:]]
fps={s['fullRunFingerprint'] for s in summaries}
fills={s['canonicalFills'] for s in summaries}
assert len(fps)==1, fps
assert len(fills)==1 and next(iter(fills))==50, fills
assert all(s['result']=='PASS' for s in summaries)
assert all(s['reconciliation']=='CLEAN' for s in summaries)
assert all(s['routeSafe'] is True for s in summaries)
PY
pass 'all continuous/resumed/system/dashboard/paced paths converge to one accepted fingerprint'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 59 RELEASE GATE: PASS — CANONICAL REPLAY IS RELEASE-CANDIDATE READY'
printf '%s\n' '============================================================'
printf 'fingerprint=%s\n' "$EXPECTED_FP"
printf 'window=%s..%s days=%s canonicalFills=%s RealTest=%s/%s differences=0\n' \
  "$START" "$END" "$EXPECTED_DAYS" "$EXPECTED_FILLS" "$EXPECTED_TRADES" "$EXPECTED_TRADES"
printf '%s\n' 'Validated: direct RealTest parity, deterministic rerun, system restart, dashboard==system, dashboard restart, paced-speed invariance.'
printf '%s\n' 'The final long/slow browser visual acceptance remains intentionally separate.'
