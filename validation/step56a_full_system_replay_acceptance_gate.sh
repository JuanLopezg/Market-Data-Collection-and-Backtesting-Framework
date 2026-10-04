#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/historical_replay"
ART="$ROOT/docs/venue/step56a"
CSV="$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv"
OUT="$ROOT/deploy/historical_replay/run/step56a_full_system_replay_summary.json"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP56A: PASS: %s\n' "$*"; }
fail(){ printf 'STEP56A: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 56A — FULL-SYSTEM REPLAY ACCEPTANCE CAMPAIGN'
printf '%s\n' '============================================================'
printf '%s\n' 'Real historical OHLCV + TimeHandler + PureRSI + Risk + Planner + canonical MOCK.'
printf '%s\n' 'Fast 107-day acceptance window; not the full 2020-2025 history.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step56 runtime remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step56/SHA256SUMS >/dev/null) || fail 'Step56 frozen artifact changed'
pass 'Step56 runtime/adapter contract remains byte-for-byte frozen'

printf '%s\n' '[2/8] Historical dataset, PureRSI, portfolio config and explicit mapping are exact'
python3 -S - "$ROOT" <<'PYDATA'
import csv,hashlib,json,pathlib,sys
r=pathlib.Path(sys.argv[1]); c=json.loads((r/'config/historical_replay/step56a_full_system_replay_acceptance_v1.json').read_text())
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
assert sha(r/c['dataset']['path'])==c['dataset']['sha256']
assert sha(r/c['sourceMapping']['path'])==c['sourceMapping']['sha256']
assert sha(r/c['strategy']['configPath'])==c['strategy']['sha256']
assert sha(r/c['portfolioRisk']['configPath'])==c['portfolioRisk']['sha256']
with (r/c['sourceMapping']['path']).open(newline='') as f: rows=list(csv.DictReader(f))
assert len(rows)==150==c['sourceMapping']['entries']
assert len({x['source_symbol'] for x in rows})==150
assert len({x['canonical_asset'] for x in rows})==150
assert c['sourceMapping']['policy']=='EXPLICIT_EXACT_ONLY_NO_RUNTIME_SUFFIX_OR_ALIAS_HEURISTIC'
assert c['strategy']['type']=='PureRSI' and c['strategy']['maxActiveSignals']==10
assert c['strategy']['universe']=='TOP_20_BY_SMA_VOLUME_25'
assert c['strategy']['ranker']=='RSI_CLOSE_7_DESC'
assert c['strategy']['rsiEntry']==80.0 and c['strategy']['rsiExit']==70.0
assert c['portfolioRisk']['equalWeightPerFullSignal']==0.10
assert c['portfolioRisk']['rebalance']=='ENTRY_EXIT_ONLY'
PYDATA
pass 'same historical source/configs are frozen and all source mappings are explicit'

printf '%s\n' '[3/8] Step56A acceptance fingerprint recomputes exactly'
python3 -S - "$ROOT" <<'PYFP'
import hashlib,json,pathlib,sys
r=pathlib.Path(sys.argv[1]); cpath=r/'config/historical_replay/step56a_full_system_replay_acceptance_v1.json'; m=json.loads((r/'config/historical_replay/step56a_full_system_replay_acceptance_manifest_v1.json').read_text()); c=json.loads(cpath.read_text())
def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()
fp=hashlib.sha256((f"step56a-v1\n{m['step56RuntimeFingerprint']}\n{m['datasetSha256']}\n{m['sourceMappingSha256']}\n{m['strategyConfigSha256']}\n{m['portfolioConfigSha256']}\n{sha(cpath)}\n").encode()).hexdigest()
assert c['boundStep56RuntimeFingerprint']==m['step56RuntimeFingerprint']
assert sha(cpath)==m['acceptanceConfigSha256']
assert fp==m['combinedAcceptanceFingerprint']
assert m['actualHistoricalWindowCampaignImplemented'] is True
assert m['fullHistoricalDatasetCampaignExecuted'] is False
assert m['timeHandlerReleaseGateUsed'] is True
assert m['privateRealVenueRoutingEnabled'] is False
PYFP
pass 'acceptance evidence contract is fingerprint-bound to Step56'

printf '%s\n' '[4/8] Step56A artifacts are present and hash-clean'
for f in \
 "$CFG/step56a_source_symbol_map_v1.csv" \
 "$CFG/step56a_full_system_replay_acceptance_v1.json" \
 "$CFG/step56a_full_system_replay_acceptance_manifest_v1.json" \
 "$ART/STEP_56A_FULL_SYSTEM_REPLAY_ACCEPTANCE.md" \
 "$ART/STEP_57_IMPLEMENTATION_HANDOFF.json" \
 "$ART/SHA256SUMS" \
 "$ROOT/tools/historical_replay/step56a_full_system_replay_acceptance.cpp"; do
 [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step56A hash mismatch'
pass 'Step56A mapping/config/code/spec/handoff match frozen hashes'

printf '%s\n' '[5/8] C++20 real PureRSI full-system acceptance runner compiles'
CXX="${CXX:-c++}"
mkdir -p "$TMP/obj"

INCLUDES=(
 -I"$ROOT/lib/src/account" -I"$ROOT/lib/src/analytics" -I"$ROOT/lib/src/common_types"
 -I"$ROOT/lib/src/contracts" -I"$ROOT/lib/src/data_types" -I"$ROOT/lib/src/exchange"
 -I"$ROOT/lib/src/execution" -I"$ROOT/lib/src/filter" -I"$ROOT/lib/src/indicator"
 -I"$ROOT/lib/src/market" -I"$ROOT/lib/src/portfolio" -I"$ROOT/lib/src/position"
 -I"$ROOT/lib/src/ranker" -I"$ROOT/lib/src/rebalance" -I"$ROOT/lib/src/risk"
 -I"$ROOT/lib/src/runtime" -I"$ROOT/lib/src/signal" -I"$ROOT/lib/src/sizing"
 -I"$ROOT/lib/src/strategy" -I"$ROOT/lib/src/strategy/strategies" -I"$ROOT/lib/src/universe"
)

SOURCES=(
 tools/historical_replay/step56a_full_system_replay_acceptance.cpp
 lib/src/runtime/strategy_signal_engine.cpp
 lib/src/runtime/portfolio_risk_engine.cpp
 lib/src/runtime/notional_order_planner_engine.cpp
 lib/src/runtime/rolling_market_state.cpp
 lib/src/runtime/time_handler.cpp
 lib/src/strategy/strategy.cpp
 lib/src/universe/universe_selector.cpp
 lib/src/universe/liquidity_universe.cpp
 lib/src/ranker/ranker.cpp
 lib/src/ranker/indicator_ranker.cpp
 lib/src/indicator/indicator_engine.cpp
 lib/src/indicator/indicator_calculators.cpp
 lib/src/indicator/indicator_spec.cpp
)

compile_one(){
 local src="$1"
 local obj="$TMP/obj/$(printf '%s' "$src" | tr '/.' '__').o"
 "$CXX" -std=c++20 -Wall -Wextra -Werror -pedantic "${INCLUDES[@]}" -c "$ROOT/$src" -o "$obj"
}

JOBS="${STEP56A_COMPILE_JOBS:-8}"
for src in "${SOURCES[@]}"; do
 compile_one "$src" &
 while (( $(jobs -rp | wc -l) >= JOBS )); do
  wait -n
 done
done
wait

"$CXX" "$TMP"/obj/*.o -pthread -o "$TMP/step56a_runner"
pass 'production Strategy/Risk/Planner/TimeHandler + canonical MOCK runner compiles cleanly'

printf '%s\n' '[6/8] Actual frozen historical window runs end-to-end'
rm -f "$OUT"
"$TMP/step56a_runner" \
 --csv "$CSV" \
 --mapping "$CFG/step56a_source_symbol_map_v1.csv" \
 --durable "$TMP/mock-state" \
 --output "$OUT" \
 --start 2020-01-01 \
 --end 2020-04-16 \
 --speed 100000000 >"$TMP/run.log" 2>&1 || {
 cat "$TMP/run.log" >&2
 fail 'full-system historical acceptance runner failed'
}
cat "$TMP/run.log"
grep -Fq 'STEP56A_RESULT=PASS' "$TMP/run.log" || fail 'runner PASS marker missing'
pass '107 real historical days traversed the full production-engine pipeline'

printf '%s\n' '[7/8] Runtime evidence exactly matches the frozen clean-room baseline'
python3 -S - "$CFG/step56a_full_system_replay_acceptance_v1.json" "$OUT" <<'PYBASE'
import json,math,pathlib,sys
c=json.loads(pathlib.Path(sys.argv[1]).read_text()); got=json.loads(pathlib.Path(sys.argv[2]).read_text()); exp=c['expectedBaseline']
assert got['result']=='PASS'
assert got['firstDate']==c['dataset']['acceptanceStartDate'] and got['lastDate']==c['dataset']['acceptanceEndDate']
for k,v in exp.items():
    if isinstance(v,float): assert math.isclose(got[k],v,rel_tol=0.0,abs_tol=1e-9),(k,got[k],v)
    else: assert got[k]==v,(k,got[k],v)
assert got['reconciliation']=='CLEAN' and got['reconciliationIssues']==0 and got['routeSafe'] is True
assert got['canonicalFills']>0 and got['canonicalAccountingEvents']==got['canonicalFills']
PYBASE
pass 'orders/fills/accounting/ledger/reconciliation and all deterministic fingerprints match exactly'

printf '%s\n' '[8/8] Step57 handoff is bound to Step56A evidence and real/private routing remains disabled'
python3 -S - "$CFG/step56a_full_system_replay_acceptance_manifest_v1.json" "$ART/STEP_57_IMPLEMENTATION_HANDOFF.json" <<'PYHANDOFF'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text()); h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step56a-to-step57-v1' and h['nextStep']=='STEP_57_MANUAL_CONTROL_TO_MOCK_NORMAL_PIPELINE'
assert h['step56aAcceptanceFingerprint']==m['combinedAcceptanceFingerprint']
assert 'private Hyperliquid auth/signing/routing' in h['forbiddenUntilLater']
assert h['afterStep57']=='STEP_58_DASHBOARD_SIMULATION_COMPLETION'
PYHANDOFF
pass 'next safe step is Step57 Manual Control -> MOCK normal pipeline'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 56A: PASS — FULL-SYSTEM REPLAY ACCEPTANCE VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Real historical OHLCV drove TimeHandler -> PureRSI -> Risk -> Planner -> CanonicalVenueAdapter -> MOCK -> Fill -> Accounting -> Ledger/Reconciliation.'
printf '%s\n' 'The deterministic Step56A evidence matched the frozen clean-room baseline exactly.'
printf '%s\n' 'Next safe step: Step57 Manual Control -> MockExchange normal pipeline.'
