#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
CFG="$ROOT/config/historical_replay"
ART="$ROOT/docs/venue/step58"
RUN_DIR="$ROOT/deploy/historical_replay/run/step58_dashboard"
BIN="$RUN_DIR/bin/step58_dashboard_simulation_runner"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

pass(){ printf 'STEP58: PASS: %s\n' "$*"; }
fail(){ printf 'STEP58: FAIL: %s\n' "$*" >&2; exit 1; }

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 58 — DASHBOARD SIMULATION COMPLETION IMPLEMENTATION GATE'
printf '%s\n' '============================================================'
printf '%s\n' 'Compiles/tests the bridge and runner. Visual browser acceptance is performed after this gate.'
printf '\n'

printf '%s\n' '[1/8] Frozen Step56A + Step57 evidence remains hash-clean'
(cd "$ROOT" && sha256sum -c docs/venue/step56a/SHA256SUMS >/dev/null) || fail 'Step56A frozen artifact changed'
(cd "$ROOT" && sha256sum -c docs/venue/step57/SHA256SUMS >/dev/null) || fail 'Step57 frozen artifact changed'
pass 'historical full-system acceptance and manual MOCK pipeline remain frozen'

printf '%s\n' '[2/8] Step58 artifacts are present and hash-clean'
for f in \
 "$CFG/step58_dashboard_simulation_v1.json" \
 "$CFG/step58_dashboard_simulation_manifest_v1.json" \
 "$ART/STEP_58_DASHBOARD_SIMULATION_COMPLETION.md" \
 "$ART/STEP_59_IMPLEMENTATION_HANDOFF.json" \
 "$ART/SHA256SUMS" \
 "$ROOT/dashboard/docker-compose.simulation.yml" \
 "$ROOT/dashboard/dashboard-api/internal/provider/simulation.go" \
 "$ROOT/dashboard/dashboard-api/internal/provider/simulation_test.go" \
 "$ROOT/tools/historical_replay/build_step58_dashboard_simulation_runner.sh" \
 "$ROOT/tools/historical_replay/run_step58_dashboard_simulation.sh" \
 "$ROOT/tools/historical_replay/step58_dashboard_simulation_runner.cpp"; do
  [[ -f "$f" ]] || fail "missing ${f#$ROOT/}"
done
(cd "$ROOT" && sha256sum -c "$ART/SHA256SUMS" >/dev/null) || fail 'Step58 hash mismatch'
pass 'Step58 code/config/docs/dashboard changes match frozen hashes'

printf '%s\n' '[3/8] Step58 fingerprint binds to exact Step56A + Step57 fingerprints'
python3 -S - "$CFG/step56a_full_system_replay_acceptance_manifest_v1.json" "$ROOT/config/manual_control/step57_mock_manual_pipeline_manifest_v1.json" "$CFG/step58_dashboard_simulation_v1.json" "$CFG/step58_dashboard_simulation_manifest_v1.json" <<'PY'
import hashlib,json,pathlib,sys
s56=json.loads(pathlib.Path(sys.argv[1]).read_text())
s57=json.loads(pathlib.Path(sys.argv[2]).read_text())
raw=pathlib.Path(sys.argv[3]).read_bytes()
cfg=json.loads(raw.decode())
m=json.loads(pathlib.Path(sys.argv[4]).read_text())
sha=hashlib.sha256(raw).hexdigest()
fp=hashlib.sha256(
    f"step58-v1\n{s56['combinedAcceptanceFingerprint']}\n{s57['combinedManualControlFingerprint']}\n{sha}\n".encode()
).hexdigest()
assert cfg['boundStep56AcceptanceFingerprint']==s56['combinedAcceptanceFingerprint']
assert cfg['boundStep57ManualControlFingerprint']==s57['combinedManualControlFingerprint']
assert cfg['stateContract']['browserTransport']=='AUTHENTICATED_REST_PLUS_SSE_INVALIDATION'
assert cfg['stateContract']['ssePayloadCarriesTradingData'] is False
assert cfg['timeSemantics']['openPhase']=='OPEN_T_PLUS_1_ONLY_NO_FUTURE_CLOSE'
assert cfg['timeSemantics']['wallClockEconomics'] is False
assert cfg['manualControl']['browserDirectVenue'] is False
assert cfg['manualControl']['realProviderEnabled'] is False
assert cfg['liveVsExpected']['fabricatedMetrics'] is False
assert m['simulationConfigSha256']==sha
assert m['step56AcceptanceFingerprint']==s56['combinedAcceptanceFingerprint']
assert m['step57ManualControlFingerprint']==s57['combinedManualControlFingerprint']
assert m['combinedDashboardSimulationFingerprint']==fp
assert m['dashboardPrivateKeysAdded'] is False
assert m['dashboardDirectVenueAccessAdded'] is False
assert m['privateRealVenueRoutingEnabled'] is False
assert m['visualAcceptanceExecutedByUser'] is False
PY
pass 'Step58 configuration/fingerprint is reproducible and dependency-bound'

printf '%s\n' '[4/8] Dashboard API simulation provider/server tests pass'
(
  cd "$ROOT/dashboard/dashboard-api"
  GOTOOLCHAIN=local go test ./internal/provider ./internal/server ./cmd/dashboard-api -count=1
) >"$TMP/go-test.log" 2>&1 || { cat "$TMP/go-test.log" >&2; fail 'Go dashboard simulation tests failed'; }
pass 'Go provider, route boundary, server and configured-provider packages compile/test'

printf '%s\n' '[5/8] Browser boundary, SSE invalidation and frontend/compose contracts remain safe'
grep -Fq 'case "simulation":' "$ROOT/dashboard/dashboard-api/cmd/dashboard-api/main.go" || fail 'simulation provider mode missing'
grep -Fq 'case "simulation.step58.generation":' "$ROOT/dashboard/dashboard-api/internal/server/stream.go" || fail 'simulation SSE invalidation missing'
grep -Fq 'RouteManualControl(context.Context, provider.SimulationManualRouteRequest)' "$ROOT/dashboard/dashboard-api/internal/server/server.go" || fail 'simulation manual transport boundary missing'
grep -Fq 'Step 58 · MOCK Simulation Routing' "$ROOT/dashboard/src/pages/ManualControlPage.tsx" || fail 'frontend Step58 manual contract label missing'
grep -Fq 'DASHBOARD_DATA_PROVIDER: "simulation"' "$ROOT/dashboard/docker-compose.simulation.yml" || fail 'simulation compose provider missing'
if grep -Ein 'PRIVATE_KEY|MNEMONIC|SEED_PHRASE|HYPERLIQUID.*SECRET|NATS_URL|POSTGRES.*PASSWORD' "$ROOT/dashboard/docker-compose.simulation.yml" >"$TMP/unsafe-compose"; then
  cat "$TMP/unsafe-compose" >&2
  fail 'private/trading credential material leaked into simulation dashboard overlay'
fi
pass 'browser remains REST/SSE dashboard-only; simulation overlay adds no private trading credentials'

printf '%s\n' '[6/8] C++20 production-engine simulation runner compiles with warnings as errors'
mkdir -p "$RUN_DIR/bin"
STEP58_BUILD_JOBS="${STEP58_BUILD_JOBS:-8}" bash "$ROOT/tools/historical_replay/build_step58_dashboard_simulation_runner.sh" "$ROOT" "$BIN" >"$TMP/build.log" 2>&1 || { cat "$TMP/build.log" >&2; fail 'Step58 C++ runner build failed'; }
[[ -x "$BIN" ]] || fail 'Step58 runner binary missing after build'
pass 'PureRSI/Risk/Planner/TimeHandler/canonical MOCK dashboard runner compiled'

printf '%s\n' '[7/8] Short replay publishes no-lookahead OPEN state and ends CLEAN/MANUAL_READY'
SMOKE="$TMP/smoke"
mkdir -p "$SMOKE"
"$BIN" \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$SMOKE/durable" --state-dir "$SMOKE/state" \
  --start 2020-01-01 --end 2020-01-01 --speed 100000000 \
  --ui-delay-ms 500 --exit-after-replay >"$SMOKE/runner.log" 2>&1 &
SMOKE_PID=$!
python3 -S - "$SMOKE/state/state.json" <<'PY'
import json,pathlib,sys,time
p=pathlib.Path(sys.argv[1])
seen=False
for _ in range(100):
    if p.exists():
        try:
            d=json.loads(p.read_text())
            if d.get('phase')=='REPLAY_OPEN':
                assert d['market']
                for b in d['market']:
                    assert b['high']==b['open']==b['low']==b['close']
                    assert b['volume']==0
                seen=True
                break
        except (json.JSONDecodeError,KeyError,AssertionError):
            pass
    time.sleep(.02)
assert seen, 'REPLAY_OPEN state was not observed'
PY
wait "$SMOKE_PID" || { cat "$SMOKE/runner.log" >&2; fail 'short replay runner failed'; }
python3 -S - "$SMOKE/state/state.json" <<'PY'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
assert s['phase']=='MANUAL_READY'
assert s['venueId']=='MOCK' and s['environment']=='MOCK'
assert s['reconciliation']['state']=='CLEAN'
assert s['manual']['ready'] is True
assert s['routeSafe'] is True
assert s['ledger']['valid'] is True
assert s['step57ManualFingerprint']=='65c0a7b418d7f3873f38f6a1daa87d5d915e4a254452865b79a69df24c3dc5f0'
PY
pass 'OPEN publishes no future close; completed replay hands off flat/CLEAN to manual simulation safely'

printf '%s\n' '[8/8] Manual file transport reaches Step57 normal pipeline and returns canonical Fill/Reconciliation'
E2E="$TMP/manual-e2e"
mkdir -p "$E2E"
"$BIN" \
  --csv "$ROOT/deploy/historical_replay/run/1d_cmc_by_date.csv" \
  --mapping "$ROOT/config/historical_replay/step56a_source_symbol_map_v1.csv" \
  --durable "$E2E/durable" --state-dir "$E2E/state" \
  --start 2020-01-01 --end 2020-01-02 --speed 100000000 \
  --ui-delay-ms 0 --manual-once --manual-idle-timeout-ms 15000 >"$E2E/runner.log" 2>&1 &
E2E_PID=$!
python3 -S - "$E2E/state/state.json" <<'PY'
import json,os,pathlib,sys,time
p=pathlib.Path(sys.argv[1])
for _ in range(300):
    if p.exists():
        try:
            d=json.loads(p.read_text())
            if d.get('phase')=='MANUAL_READY' and d.get('manual',{}).get('ready'):
                break
        except json.JSONDecodeError:
            pass
    time.sleep(.02)
else:
    raise AssertionError('MANUAL_READY timeout')
req=p.parent/'requests'; req.mkdir(exist_ok=True)
body='\n'.join([
 'STEP58_MANUAL_V1',
 'request_id=step58-gate-manual',
 'correlation_id=step58-gate-manual',
 'actor=step58-gate',
 'request_hash=sha256:step58-gate',
 f"decision_timestamp={d['manual']['decisionTimestamp']}",
 f"execution_timestamp={d['manual']['executionTimestamp']}",
 f"reference_generation={d['generation']}",
 'target=BTCUSDT,0.100000000000',
 'cash=0.900000000000',
 ''
])
tmp=req/'.gate.tmp'; tmp.write_text(body); os.replace(tmp,req/'step58-gate-manual.request')
PY
wait "$E2E_PID" || { cat "$E2E/runner.log" >&2; fail 'manual transport runner failed'; }
python3 -S - "$E2E/state/state.json" "$ART/STEP_59_IMPLEMENTATION_HANDOFF.json" "$CFG/step58_dashboard_simulation_manifest_v1.json" <<'PY'
import json,pathlib,sys
s=json.loads(pathlib.Path(sys.argv[1]).read_text())
h=json.loads(pathlib.Path(sys.argv[2]).read_text())
m=json.loads(pathlib.Path(sys.argv[3]).read_text())
assert s['manual']['lastStatus']=='SUBMITTED'
assert s['manual']['ready'] is True
assert s['reconciliation']['state']=='CLEAN'
assert s['ledger']['valid'] is True
assert len(s['fills'])>=1
assert any(p['asset']=='BTCUSDT' and p['quantity']>0 for p in s['account']['positions'])
assert h['contractVersion']=='step58-to-step59-v1'
assert h['nextStep']=='STEP_59_DETERMINISTIC_EVIDENCE_SPEED_INVARIANCE_RESTART'
assert h['step58DashboardSimulationFingerprint']==m['combinedDashboardSimulationFingerprint']
PY
pass 'dashboard trading-control transport invokes Step57 MOCK path; Fill/ledger/reconciliation stay canonical and Step59 handoff is bound'

printf '\n'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 58 IMPLEMENTATION: PASS — DASHBOARD SIMULATION BRIDGE VALIDATED'
printf '%s\n' '============================================================'
printf '%s\n' 'Code/tests are ready. Step58 closes only after browser visual acceptance with the real replay running.'
printf '%s\n' 'Next after visual acceptance: Step59 Deterministic Evidence / Speed Invariance / Restart.'
