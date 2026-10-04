#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/venues/mock"
ART="$ROOT/docs/venue/step56"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP56: PASS: %s\n' "$*"; }
fail(){ printf 'STEP56: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 56 — FULL-SYSTEM REPLAY RUNTIME WIRING'
printf '%s\n' '============================================================'
printf '%s\n' 'Wiring gate only. The actual large historical replay campaign runs after this gate.'
printf '\n'

printf '%s\n' '[1/8] Step55 prerequisite gate passes'
bash "$ROOT/validation/step55_fault_chaos_rate_limit_gate.sh" "$ROOT" >"$TMP/step55.log" 2>&1 || {
  cat "$TMP/step55.log" >&2
  fail 'Step55 prerequisite failed'
}
grep -Fq 'STEP 55: PASS — FAULT / CHAOS / RATE-LIMIT ENGINE VALIDATED' "$TMP/step55.log" \
  || fail 'Step55 PASS banner missing'
pass 'Step55 fault/chaos/rate-limit dependency is validated'

printf '%s\n' '[2/8] Step56 artifacts are present and hash-clean'
for f in \
 "$CFG/full_system_replay_runtime_v1.json" \
 "$CFG/full_system_replay_runtime_manifest_v1.json" \
 "$ART/STEP_56_FULL_SYSTEM_REPLAY_RUNTIME.md" \
 "$ART/STEP_56_REPLAY_ACCEPTANCE_HANDOFF.json" \
 "$ART/SHA256SUMS" \
 "$ROOT/lib/src/exchange/mock_exchange_adapter_v1.h" \
 "$ROOT/lib/src/runtime/full_system_replay_fingerprints_v1.h" \
 "$ROOT/lib/src/runtime/full_system_replay_runtime_v1.h" \
 "$ROOT/validation/step56_full_system_replay_runtime_test.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step56 hash mismatch'
pass 'Step56 config/code/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step56 fingerprint binds to exact Step55 fingerprint'
python3 -S - "$CFG/fault_chaos_rate_limit_manifest_v1.json" "$CFG/full_system_replay_runtime_v1.json" "$CFG/full_system_replay_runtime_manifest_v1.json" <<'PY'
import hashlib,json,pathlib,sys
s55=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes()
cfg=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
    f"step56-v1\n{s55['combinedFaultChaosFingerprint']}\n"
    f"full-system-replay-runtime-v1-step56\n{sha}\n".encode()
).hexdigest()
assert cfg['boundStep55FaultChaosFingerprint']==s55['combinedFaultChaosFingerprint']
assert m['step55FaultChaosFingerprint']==s55['combinedFaultChaosFingerprint']
assert m['runtimeConfigSha256']==sha
assert m['combinedFullSystemRuntimeFingerprint']==fp
assert m['canonicalMockExchangeAdapterImplemented'] is True
assert m['strategySignalEngineWired'] is True
assert m['portfolioRiskEngineWired'] is True
assert m['notionalOrderPlannerWired'] is True
assert m['canonicalVenueOrderPathWired'] is True
assert m['fillDrivenStrategyPositionMirrorImplemented'] is True
assert m['reconciliationGateWired'] is True
assert m['actualHistoricalDatasetCampaignExecuted'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_56_FULL_SYSTEM_REPLAY_ACCEPTANCE_CAMPAIGN'
PY
pass 'runtime fingerprint is reproducible and bound to Step55'

printf '%s\n' '[4/8] Full pipeline/no-lookahead/canonical-edge policies are explicit'
python3 -S - "$CFG/full_system_replay_runtime_v1.json" <<'PY'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['historicalReleaseContract']['closedSlice']=='T_CLOSE'
assert d['historicalReleaseContract']['executionOpen']=='T_PLUS_1_OPEN'
assert d['historicalReleaseContract']['businessTimeSource']=='UPSTREAM_TIMEHANDLER_RELEASE_TIMESTAMP'
assert d['historicalReleaseContract']['wallClockEconomics'] is False
assert d['explicitVenueSelection']=='MOCK_ONLY'
assert d['venueFallback'] is False
assert d['smartOrSplitRouting'] is False
assert d['plannerToVenueEdge']['notionalQuantityReference']=='CLOSE_T'
assert d['plannerToVenueEdge']['executionLimitReference']=='OPEN_T_PLUS_1'
assert d['plannerToVenueEdge']['minimumAndSemanticAdmissionAuthority']=='STEP50_CANONICAL_MOCK_ADMISSION'
assert d['submitBatching']=='ONE_CANONICAL_BATCH_PER_EXECUTION_OPEN'
assert d['newExposureGate']=='STEP55_CAN_ROUTE_NEW_SUBMIT'
assert d['fillAuthority']=='CANONICAL_STEP51_FILL_ONLY'
assert d['actualHistoricalDatasetCampaignExecuted'] is False
assert d['privateRealVenueRoutingEnabled'] is False
PY
pass 'close(T)->later open(T+1), explicit MOCK routing and Fill-only execution authority are frozen'

printf '%s\n' '[5/8] C++20 full-system wiring campaign passes'
CXX="${CXX:-c++}"
"$CXX" -std=c++20 -Wall -Wextra -Werror -pedantic \
 -I"$ROOT/lib/src/account" \
 -I"$ROOT/lib/src/analytics" \
 -I"$ROOT/lib/src/common_types" \
 -I"$ROOT/lib/src/contracts" \
 -I"$ROOT/lib/src/data_types" \
 -I"$ROOT/lib/src/exchange" \
 -I"$ROOT/lib/src/execution" \
 -I"$ROOT/lib/src/filter" \
 -I"$ROOT/lib/src/indicator" \
 -I"$ROOT/lib/src/market" \
 -I"$ROOT/lib/src/portfolio" \
 -I"$ROOT/lib/src/position" \
 -I"$ROOT/lib/src/ranker" \
 -I"$ROOT/lib/src/rebalance" \
 -I"$ROOT/lib/src/risk" \
 -I"$ROOT/lib/src/runtime" \
 -I"$ROOT/lib/src/signal" \
 -I"$ROOT/lib/src/sizing" \
 -I"$ROOT/lib/src/strategy" \
 -I"$ROOT/lib/src/universe" \
 "$ROOT/validation/step56_full_system_replay_runtime_test.cpp" \
 "$ROOT/lib/src/runtime/strategy_signal_engine.cpp" \
 "$ROOT/lib/src/runtime/portfolio_risk_engine.cpp" \
 "$ROOT/lib/src/runtime/notional_order_planner_engine.cpp" \
 "$ROOT/lib/src/runtime/rolling_market_state.cpp" \
 "$ROOT/lib/src/strategy/strategy.cpp" \
 "$ROOT/lib/src/universe/universe_selector.cpp" \
 "$ROOT/lib/src/ranker/ranker.cpp" \
 "$ROOT/lib/src/indicator/indicator_engine.cpp" \
 "$ROOT/lib/src/indicator/indicator_calculators.cpp" \
 "$ROOT/lib/src/indicator/indicator_spec.cpp" \
 -o "$TMP/step56_test"
"$TMP/step56_test"
pass 'Strategy -> Risk -> Planner -> canonical MOCK -> Fill -> Accounting -> Recon/Ledger wiring validated'

printf '%s\n' '[6/8] Trading commands cross CanonicalVenueAdapter; no direct planner-to-MOCK submit bypass'
grep -Fq 'CanonicalVenueAdapter& venue_adapter_' "$ROOT/lib/src/runtime/full_system_replay_runtime_v1.h" \
  || fail 'canonical adapter boundary missing'
grep -Fq 'venue_adapter_.submitOrders(batch)' "$ROOT/lib/src/runtime/full_system_replay_runtime_v1.h" \
  || fail 'submit does not cross CanonicalVenueAdapter'
grep -Fq 'venue_adapter_.cancelOrders(batch)' "$ROOT/lib/src/runtime/full_system_replay_runtime_v1.h" \
  || fail 'cancel does not cross CanonicalVenueAdapter'
if grep -Ein 'runtime\(\)\.submit\(|lifecycle\(\)\.submit\(|chaos\(\)\.submit\(' \
 "$ROOT/lib/src/runtime/full_system_replay_runtime_v1.h" >"$TMP/bypass"; then
 cat "$TMP/bypass" >&2
 fail 'direct MOCK submit bypass detected in full-system runtime'
fi
pass 'Strategy/Risk/Planner cannot bypass the canonical venue command boundary'

printf '%s\n' '[7/8] Step56 introduces no wall-clock economics/private real routing'
if grep -Ein 'system_clock|steady_clock|high_resolution_clock|sleep_for|sleep_until|gettimeofday|clock_gettime|std::time|(^|[^[:alnum:]_])time\(' \
 "$ROOT/lib/src/runtime/full_system_replay_runtime_v1.h" \
 "$ROOT/lib/src/exchange/mock_exchange_adapter_v1.h" >"$TMP/clock"; then
 cat "$TMP/clock" >&2
 fail 'wall/monotonic time leaked into Step56 business/economic path'
fi
if grep -Ein 'hyperliquid|private[_ -]?key|mnemonic|seed phrase|api wallet|smart[_ -]?order[_ -]?routing' \
 "$ROOT/lib/src/runtime/full_system_replay_runtime_v1.h" \
 "$ROOT/lib/src/exchange/mock_exchange_adapter_v1.h" >"$TMP/private"; then
 cat "$TMP/private" >&2
 fail 'private real-venue/smart-routing material leaked into Step56'
fi
pass 'business time remains release-supplied and private real venue stays disabled'

printf '%s\n' '[8/8] Replay-acceptance handoff is bound to exact Step56 fingerprint'
python3 -S - "$CFG/full_system_replay_runtime_manifest_v1.json" "$ART/STEP_56_REPLAY_ACCEPTANCE_HANDOFF.json" <<'PY'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step56-runtime-to-replay-acceptance-v1'
assert h['nextAction']=='STEP_56_FULL_SYSTEM_REPLAY_ACCEPTANCE_CAMPAIGN'
assert h['step56RuntimeFingerprint']==m['combinedFullSystemRuntimeFingerprint']
assert h['step55FaultChaosFingerprint']==m['step55FaultChaosFingerprint']
assert 'route every trading command through CanonicalVenueAdapter -> MockExchangeAdapterV1' in h['requiredAcceptance']
assert h['afterAcceptanceRoadmap']=='STEP_57_MANUAL_CONTROL_TO_MOCK_NORMAL_PIPELINE'
PY
pass 'after this gate the next action is the actual full-system historical replay campaign'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 56: PASS — FULL-SYSTEM REPLAY RUNTIME WIRING VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Strategy/Risk/Planner now route through CanonicalVenueAdapter -> MOCK and canonical Fill drives downstream state.'
printf '%s\n' 'This gate validates wiring, not the large historical dataset campaign.'
printf '%s\n' 'Next action: Step56 Full-System Replay Acceptance Campaign.'
