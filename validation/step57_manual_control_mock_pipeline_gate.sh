#!/usr/bin/env bash
set -euo pipefail
ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/manual_control"
ART="$ROOT/docs/venue/step57"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
pass(){ printf 'STEP57: PASS: %s\n' "$*"; }
fail(){ printf 'STEP57: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 57 — MANUAL CONTROL -> MOCK NORMAL PIPELINE'
printf '%s\n' '============================================================'
printf '%s\n' 'Core trading-control pipeline only; dashboard transport remains Step58.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step56 + Step56A evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step56/SHA256SUMS >/dev/null) || fail 'Step56 frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step56a/SHA256SUMS >/dev/null) || fail 'Step56A frozen artifact changed'
pass 'Step56 runtime and real historical acceptance artifacts remain byte-for-byte frozen'

printf '%s\n' '[2/8] Step57 artifacts are present and hash-clean'
for f in \
 "$CFG/step57_mock_manual_pipeline_v1.json" \
 "$CFG/step57_mock_manual_pipeline_manifest_v1.json" \
 "$ART/STEP_57_MANUAL_CONTROL_TO_MOCK_NORMAL_PIPELINE.md" \
 "$ART/STEP_58_IMPLEMENTATION_HANDOFF.json" \
 "$ART/SHA256SUMS" \
 "$ROOT/lib/src/contracts/manual_target_intent_v1.h" \
 "$ROOT/lib/src/runtime/manual_portfolio_risk_engine_v1.h" \
 "$ROOT/lib/src/runtime/manual_control_mock_pipeline_v1.h" \
 "$ROOT/tools/manual_control/step57_mock_manual_pipeline_cli.cpp" \
 "$ROOT/validation/step57_manual_control_mock_pipeline_test.cpp"; do
 [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step57 hash mismatch'
pass 'Step57 contract/risk/pipeline/CLI/spec/test match frozen hashes'

printf '%s\n' '[3/8] Step57 fingerprint binds to exact Step56A acceptance fingerprint'
python3 -S - "$ROOT/config/historical_replay/step56a_full_system_replay_acceptance_manifest_v1.json" "$CFG/step57_mock_manual_pipeline_v1.json" "$CFG/step57_mock_manual_pipeline_manifest_v1.json" <<'PY2'
import hashlib,json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
raw=pathlib.Path(sys.argv[2]).read_bytes(); c=json.loads(raw); m=json.loads(pathlib.Path(sys.argv[3]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(f"step57-v1\n{s['combinedAcceptanceFingerprint']}\nmanual-control-mock-normal-pipeline-v1-step57\n{sha}\n".encode()).hexdigest()
assert c['boundStep56AcceptanceFingerprint']==s['combinedAcceptanceFingerprint']
assert m['step56AcceptanceFingerprint']==s['combinedAcceptanceFingerprint']
assert m['manualControlConfigSha256']==sha
assert m['combinedManualControlFingerprint']==fp
assert m['manualIntentContractImplemented'] is True
assert m['manualPortfolioRiskTransformationImplemented'] is True
assert m['productionNotionalPlannerUsed'] is True
assert m['canonicalVenueAdapterPathUsed'] is True
assert m['mockVenueExecutionUsed'] is True
assert m['dashboardHttpTransportEnabled'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['nextStep']=='STEP_58_DASHBOARD_SIMULATION_COMPLETION'
PY2
pass 'manual-control fingerprint is reproducible and bound to Step56A'

printf '%s\n' '[4/8] Manual risk/planner/routing policy is explicit and fail-closed'
python3 -S - "$CFG/step57_mock_manual_pipeline_v1.json" <<'PY2'
import json,pathlib,sys
d=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert d['venueId']=='MOCK' and d['environment']=='MOCK'
assert d['targetContract']['weights']=='LONG_ONLY_FRACTIONS_PLUS_EXPLICIT_CASH'
assert d['risk']['engine']=='ManualPortfolioRiskEngineV1'
assert d['risk']['unmappedAssetPolicy']=='REJECT_FAIL_CLOSED'
assert d['risk']['constraintMutationPolicy']=='REJECT_IF_REQUEST_WOULD_BE_CHANGED'
assert d['planning']['engine']=='NotionalOrderPlannerEngine'
assert d['planning']['decisionReference']=='CLOSE_T'
assert d['planning']['executionReference']=='OPEN_T_PLUS_1'
assert d['routing']['boundary']=='CanonicalVenueAdapter'
assert d['routing']['venueFallback'] is False
assert d['routing']['smartOrSplitRouting'] is False
assert d['fillAuthority']=='CANONICAL_STEP51_FILL_ONLY'
assert d['dashboardHttpTransportEnabled'] is False
assert d['realVenueRoutingEnabled'] is False
PY2
pass 'confirmed target -> manual risk -> production planner -> canonical MOCK policy is frozen'

printf '%s\n' '[5/8] C++20 manual-control normal-pipeline suite passes'
CXX="${CXX:-c++}"
INCLUDES=()
for d in account analytics common_types contracts data_types exchange execution filter indicator market portfolio position ranker rebalance risk runtime signal sizing strategy universe utils; do
 INCLUDES+=("-I$ROOT/lib/src/$d")
done
"$CXX" -std=c++20 -Wall -Wextra -Werror -pedantic "${INCLUDES[@]}" \
 "$ROOT/validation/step57_manual_control_mock_pipeline_test.cpp" \
 "$ROOT/lib/src/runtime/notional_order_planner_engine.cpp" \
 -o "$TMP/step57_test"
"$TMP/step57_test"
pass 'manual target entry/noop/flat traverses risk -> planner -> canonical MOCK -> fills/accounting/reconciliation'

printf '%s\n' '[6/8] CLI trading-control harness compiles and uses the same Step57 pipeline'
"$CXX" -std=c++20 -Wall -Wextra -Werror -pedantic "${INCLUDES[@]}" \
 "$ROOT/tools/manual_control/step57_mock_manual_pipeline_cli.cpp" \
 "$ROOT/lib/src/runtime/notional_order_planner_engine.cpp" \
 -o "$TMP/step57_cli"
"$TMP/step57_cli" "$TMP/cli-state" BTCUSDT 25 100 >"$TMP/cli.out"
grep -q 'status=1 submits=1' "$TMP/cli.out" || { cat "$TMP/cli.out" >&2; fail 'CLI did not submit through Step57 pipeline'; }
pass 'dedicated trading-control harness reaches MOCK normal pipeline without dashboard/browser exchange access'

printf '%s\n' '[7/8] No direct planner-to-MOCK/private real-venue bypass slipped in'
grep -Fq 'CanonicalVenueAdapter& venue_' "$ROOT/lib/src/runtime/manual_control_mock_pipeline_v1.h" || fail 'canonical venue boundary missing'
grep -Fq 'venue_.submitOrders(batch)' "$ROOT/lib/src/runtime/manual_control_mock_pipeline_v1.h" || fail 'manual submit does not cross canonical adapter'
if grep -Ein 'runtime\(\)\.submit\(|lifecycle\(\)\.submit\(|chaos\(\)\.submit\(|hyperliquid|private[_ -]?key|mnemonic|seed phrase|api wallet|smart[_ -]?order' \
 "$ROOT/lib/src/runtime/manual_control_mock_pipeline_v1.h" "$ROOT/lib/src/runtime/manual_portfolio_risk_engine_v1.h" >"$TMP/bypass"; then
 cat "$TMP/bypass" >&2
 fail 'direct MOCK/private/smart-routing bypass detected'
fi
pass 'manual commands cross canonical adapter; private real routing remains absent'

printf '%s\n' '[8/8] Step58 handoff is bound to exact Step57 fingerprint'
python3 -S - "$CFG/step57_mock_manual_pipeline_manifest_v1.json" "$ART/STEP_58_IMPLEMENTATION_HANDOFF.json" <<'PY2'
import json,pathlib,sys
m=json.loads(pathlib.Path(sys.argv[1]).read_text()); h=json.loads(pathlib.Path(sys.argv[2]).read_text())
assert h['contractVersion']=='step57-to-step58-v1'
assert h['nextStep']=='STEP_58_DASHBOARD_SIMULATION_COMPLETION'
assert h['step57ManualControlFingerprint']==m['combinedManualControlFingerprint']
assert 'private Hyperliquid auth/signing/routing' in h['forbiddenUntilLater']
assert any('dashboard-api remains PostgreSQL SELECT-only' in x for x in h['requiredStep58Behavior'])
PY2
pass 'next safe step is Step58 Dashboard Simulation Completion'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 57: PASS — MANUAL CONTROL -> MOCK NORMAL PIPELINE VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Confirmed manual targets traverse manual risk -> production planner -> CanonicalVenueAdapter -> MOCK -> Fill/accounting/reconciliation.'
printf '%s\n' 'Dashboard HTTP transport and all private real-venue routing remain disabled.'
printf '%s\n' 'Next safe step: Step 58 Dashboard Simulation Completion.'
