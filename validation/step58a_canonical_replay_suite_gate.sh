#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP58A-PARTIAL: PASS: %s\n' "$*"; }
fail(){ printf 'STEP58A-PARTIAL: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 58A — CANONICAL REPLAY SUITE + PARTIAL REALTEST CUTOFF'
printf '%s\n' '============================================================'
printf '%s\n' 'One research entry point; fast/system/dashboard; --days; open-trade cutoff policy.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step56A / Step57 / Step58 artifacts remain clean'
(cd "$ROOT" && sha256sum -c docs/venue/step56a/SHA256SUMS >/dev/null) \
  || fail 'Step56A frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step57/SHA256SUMS >/dev/null) \
  || fail 'Step57 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step58/SHA256SUMS >/dev/null) \
  || fail 'Step58 frozen artifact changed'
pass 'prior full-system/manual/dashboard artifacts remain untouched'

printf '%s\n' '[2/8] One canonical public CLI exposes three modes plus partial-day controls'
for f in \
 "$ROOT/research/replay.py" \
 "$ROOT/research/src/canonical_replay.cpp" \
 "$ROOT/research/src/backtesting_main.cpp" \
 "$ROOT/research/REPLAY.md"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
python3 -m py_compile "$ROOT/research/replay.py" || fail 'research/replay.py syntax failed'
python3 "$ROOT/research/replay.py" --help >"$TMP/help.txt"
python3 "$ROOT/research/replay.py" system --help >"$TMP/system-help.txt"
python3 "$ROOT/research/replay.py" dashboard --help >"$TMP/dashboard-help.txt"
grep -Fq '{fast,system,dashboard}' "$TMP/help.txt" || fail 'three canonical modes missing'
grep -Fq -- '--days DAYS' "$TMP/system-help.txt" || fail '--days missing'
grep -Fq -- '--pace-all' "$TMP/system-help.txt" || fail '--pace-all missing'
grep -Fq -- '--visual-day-minutes' "$TMP/dashboard-help.txt" || fail 'dashboard pacing missing'
pass 'fast/system/dashboard share one CLI with --days and pacing options'

printf '%s\n' '[3/8] --days resolves to exactly N historical source days'
python3 - "$ROOT" <<'PY'
import argparse, importlib.util, pathlib, sys
root=pathlib.Path(sys.argv[1])
path=root/'research/replay.py'
spec=importlib.util.spec_from_file_location('replay',path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
assert mod.resolve_window(argparse.Namespace(start='2020-01-01',days=1)) == ('2020-01-01','2020-01-01',1)
assert mod.resolve_window(argparse.Namespace(start='2020-01-01',days=10)) == ('2020-01-01','2020-01-10',10)
assert mod.resolve_window(argparse.Namespace(start='2020-01-01',days=100)) == ('2020-01-01','2020-04-09',100)
assert mod.resolve_window(argparse.Namespace(start='2020-01-01',days=107)) == ('2020-01-01','2020-04-16',107)
PY
pass 'partial cutoff is deterministic and inclusive: N days means N source days'

printf '%s\n' '[4/8] Canonical full-system runner compiles and accepts a valid zero-trade partial campaign'
python3 - "$ROOT" <<'PY'
import importlib.util, pathlib, sys
root=pathlib.Path(sys.argv[1]); path=root/'research/replay.py'
spec=importlib.util.spec_from_file_location('replay',path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
print(mod.build_full_runner(True))
PY
RUNNER="$ROOT/deploy/historical_replay/run/research_replay/bin/algotrading_replay_full"
[[ -x "$RUNNER" ]] || fail 'compiled canonical runner missing'
PART="$TMP/partial10"
mkdir -p "$PART"
"$RUNNER" \
  --mode system \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$PART/state" \
  --output "$PART/summary.json" \
  --fills-output "$PART/fills.csv" \
  --start 2020-01-01 --end 2020-01-10 \
  --profile realtest-parity --speed 1500 >"$PART/run.log" 2>&1 || {
    cat "$PART/run.log" >&2
    fail '10-day partial full-system replay failed'
  }
python3 - "$PART/summary.json" <<'PY'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert s['result']=='PASS'
assert s['days']==10
assert s['canonicalFills']==0
assert s['canonicalAccountingEvents']==0
assert s['reconciliation']=='CLEAN'
assert s['routeSafe'] is True
PY
pass 'short no-signal windows are valid PASS evidence, not false failures'

printf '%s\n' '[5/8] 107-day system campaign is compared directly with the RealTest PureRSI CSV'
SYS="$TMP/system107"
mkdir -p "$SYS"
"$RUNNER" \
  --mode system \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$SYS/state" \
  --output "$SYS/summary.json" \
  --fills-output "$SYS/fills.csv" \
  --start 2020-01-01 --end 2020-04-16 \
  --profile realtest-parity --speed 1500 >"$SYS/run.log" 2>&1 || {
    cat "$SYS/run.log" >&2
    fail '107-day partial full-system replay failed'
  }
REALTEST_NONINTERACTIVE=1 python3 - "$ROOT" "$SYS/fills.csv" "$SYS" <<'PY'
import importlib.util,pathlib,sys
root=pathlib.Path(sys.argv[1]); fills=pathlib.Path(sys.argv[2]); out=pathlib.Path(sys.argv[3])/'realtest'
path=root/'research/replay.py'
spec=importlib.util.spec_from_file_location('replay',path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
trades=mod.trades_from_fill_csv(fills,'2020-04-16')
r=mod.compare_candidate_trades_to_realtest(
    trades,
    '2020-01-01',
    '2020-04-16',
    out,
    manual_review=False,
)
assert r['result']=='PASS'
assert r['differences']==0
assert r['knownExceptionsInCode']==0
PY
pass '107-day canonical system matches the RealTest CSV directly; no T19 whitelist/prefix is used'

printf '%s\n' '[5b/8] RealTest-parity keeps historical data intact and disables execution frictions explicitly'
grep -Fq 'value.bar.volume = canonicalHistoricalVolume(std::stod(fields[6]));' \
  "$ROOT/research/src/canonical_replay.cpp" \
  || fail 'historical Strategy volume is not preserved'
grep -Fq 'observation.bar.volume = value.bar.volume;' \
  "$ROOT/research/src/canonical_replay.cpp" \
  || fail 'execution-open observation does not preserve the historical volume value'
grep -Fq 'matching.ignore_volume_capacity = true;' \
  "$ROOT/research/src/canonical_replay.cpp" \
  || fail 'RealTest parity still uses bar volume as execution capacity'
grep -Fq 'matching.max_adverse_slippage_ppm = 0U;' \
  "$ROOT/research/src/canonical_replay.cpp" \
  || fail 'RealTest parity slippage is not disabled'
grep -Fq 'matching.fee_ppm = 0U;' \
  "$ROOT/research/src/canonical_replay.cpp" \
  || fail 'RealTest parity matching fee is not neutral'
grep -Fq 'accounting.fee_ppm = 0U;' \
  "$ROOT/research/src/canonical_replay.cpp" \
  || fail 'RealTest parity accounting fee is not neutral'
grep -Fq 'RealTest parity order remained active after execution open' \
  "$ROOT/research/src/canonical_replay.cpp" \
  || fail 'RealTest parity does not fail closed on an unfilled open order'
if grep -Eq '100000000000|0\.1\*v|10000000000' "$ROOT/research/src/canonical_replay.cpp"; then
  fail 'synthetic RealTest liquidity/volume transform is still present'
fi
python3 - "$SYS/summary.json" <<'PY58A5B'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert s['result']=='PASS'
assert s['canonicalFills']==50
assert s['canonicalAccountingEvents']==0
assert s['feesPaid']==0
assert s['reconciliation']=='CLEAN'
assert s['routeSafe'] is True
assert s['volumeTransform']=='IDENTITY'
assert s['matcherIgnoresVolumeCapacity'] is True
PY58A5B
pass 'RealTest parity uses original OHLCV, full next-open execution, zero fees/slippage, and no volume capacity'

printf '%s\n' '[5c/8] mock-default still reproduces the frozen Step56A 107-day economics'
MOCK="$TMP/mock107"
mkdir -p "$MOCK"
"$RUNNER" \
  --mode system \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$MOCK/state" \
  --output "$MOCK/summary.json" \
  --fills-output "$MOCK/fills.csv" \
  --start 2020-01-01 --end 2020-04-16 \
  --profile mock-default --speed 1500 >"$MOCK/run.log" 2>&1 || {
    cat "$MOCK/run.log" >&2
    fail '107-day mock-default replay failed'
  }
python3 - "$ROOT" "$MOCK/summary.json" <<'PY58A5C'
import json,pathlib,sys,math
root=pathlib.Path(sys.argv[1])
got=json.loads(pathlib.Path(sys.argv[2]).read_text())
ref=json.loads((root/'deploy/historical_replay/run/step56a_full_system_replay_summary.json').read_text())
assert got['result']=='PASS'
for key in ('plannedSubmits','canonicalFills','canonicalAccountingEvents','openOrders','finalPositions'):
    assert got[key]==ref[key], (key,got[key],ref[key])
assert math.isclose(got['finalEquity'],ref['finalEquity'],rel_tol=0.0,abs_tol=1e-8)
assert got['reconciliation']=='CLEAN'
assert got['routeSafe'] is True
assert got['matcherIgnoresVolumeCapacity'] is False
PY58A5C
pass 'mock-default behavior remains the frozen Step56A behavior'

printf '%s\n' '[6/8] Open trades at cutoff compare entry only; closed trades compare fully'

REALTEST_NONINTERACTIVE=1 python3 - "$ROOT" "$TMP" <<'PY'
import csv,importlib.util,pathlib,sys
root=pathlib.Path(sys.argv[1]); tmp=pathlib.Path(sys.argv[2]); path=root/'research/replay.py'
spec=importlib.util.spec_from_file_location('replay',path)
mod=importlib.util.module_from_spec(spec); spec.loader.exec_module(mod)
rt=tmp/'synthetic_realtest.csv'
headers=['Trade','Strategy','Symbol','Side','DateIn','TimeIn','QtyIn','PriceIn','DateOut','TimeOut','QtyOut','PriceOut','Reason','Bars','PctGain','Profit','PctMFE','PctMAE','Fraction','Size','Dividends']
rows=[
 ['1','PureRSI','AAA','Long','01/02/2020','open','10','100','01/03/2020','open','10','110','','1','','100','','','','',''],
 ['2','PureRSI','BBB','Long','01/04/2020','open','5','200','01/10/2020','open','5','250','','6','','250','','','','',''],
]
with rt.open('w',newline='') as f:
    w=csv.writer(f); w.writerow(headers); w.writerows(rows)
candidate=[
 {'id':1,'coin':'AAA','direction':'Long','start':'2020-01-02','start_phase':'open','end':'2020-01-03','end_phase':'open','entry':100.0,'exit':110.0,'size':10.0,'pnl':100.0,'commission':0.0,'exited':True},
 {'id':2,'coin':'BBB','direction':'Long','start':'2020-01-04','start_phase':'open','end':'2020-01-05','end_phase':'open','entry':200.0,'exit':999.0,'size':5.0,'pnl':-999.0,'commission':0.0,'exited':False},
]
r=mod.compare_candidate_trades_to_realtest(candidate,'2020-01-01','2020-01-05',tmp/'cmp',realtest_csv=rt,manual_review=False)
assert r['result']=='PASS'
assert r['differences']==0
assert r['closedTradesFullyCompared']==1
assert r['openTradesEntryOnlyCompared']==1
assert r['knownExceptionsInCode']==0
PY
pass 'cutoff policy matches the requested open-trade semantics exactly'

printf '%s\n' '[7/8] Fast research mode is date/output controllable without creating a second public replay CLI'
grep -Fq 'ALGOTRADING_REPLAY_START_DATE' "$ROOT/research/src/backtesting_main.cpp" \
  || fail 'fast Backtester start override missing'
grep -Fq 'ALGOTRADING_REPLAY_END_DATE' "$ROOT/research/src/backtesting_main.cpp" \
  || fail 'fast Backtester end override missing'
grep -Fq 'ALGOTRADING_REPLAY_TRADES_CSV' "$ROOT/research/src/backtesting_main.cpp" \
  || fail 'fast candidate trade output override missing'
grep -Fq 'ALGOTRADING_REPLAY_SKIP_INTERNAL_REALTEST' "$ROOT/research/src/backtesting_main.cpp" \
  || fail 'central RealTest ownership switch missing'
if command -v meson >/dev/null 2>&1 && [[ -f "$ROOT/meson.build" ]]; then
  # Only compile when this checkout has a configured build or can configure one with its local deps.
  if [[ -f "$ROOT/build/meson-private/coredata.dat" ]]; then
    meson compile -C "$ROOT/build" algotrading_research >"$TMP/meson-fast.log" 2>&1 || {
      cat "$TMP/meson-fast.log" >&2
      fail 'modified existing research Backtester target no longer compiles'
    }
  fi
fi
pass 'fast mode reuses the existing Backtester with internal env overrides owned by research/replay.py'

printf '%s\n' '[8/8] Direct RealTest ownership is documented in code; no frozen exception whitelist or T19-prefix acceptance remains'
grep -Fq 'No new replay engine should be added' "$ROOT/research/REPLAY.md" \
  || fail 'consolidation rule missing'
grep -Fq 'DIRECT_REALTEST_MANUAL_REVIEW_V1' "$ROOT/research/replay.py" \
  || fail 'direct RealTest manual-review policy missing'
grep -Fq 'Known exceptions in code : NONE' "$ROOT/research/replay.py" \
  || fail 'manual review does not explicitly declare zero hard-coded exceptions'
if grep -Eq 'T19_REFERENCE_FILLS|REALTEST_BASELINE|known_exception_policy|KNOWN_EXCEPTION' "$ROOT/research/replay.py"; then
  fail 'frozen T19 exception/prefix acceptance still exists in canonical replay.py'
fi
if grep -Ein 'PRIVATE_KEY|MNEMONIC|SEED_PHRASE|API_WALLET|HYPERLIQUID.*(SIGN|PRIVATE|ORDER)' \
 "$ROOT/research/replay.py" "$ROOT/research/src/canonical_replay.cpp" >"$TMP/private"; then
  cat "$TMP/private" >&2
  fail 'private real-venue concern leaked into canonical replay suite'
fi
pass 'RealTest CSV is the acceptance source; mismatches require human review instead of a code whitelist'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 58A IMPLEMENTATION GATE: PASS — DIRECT REALTEST MANUAL REVIEW READY'
printf '%s\n' '============================================================'
printf '%s\n' 'Use: python3 research/replay.py {fast|system|dashboard} --days N ...'
printf '%s\n' 'Full-history acceptance is manual: mismatches are shown one-by-one, then the user approves/rejects the complete RealTest comparison.'
