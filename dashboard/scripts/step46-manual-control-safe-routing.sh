#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
OPERATOR_USER=${DASHBOARD_GATE_USERNAME:-operator}
OPERATOR_PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
VIEWER_USER=${DASHBOARD_GATE_VIEWER_USERNAME:-viewer}
VIEWER_PASS=${DASHBOARD_GATE_VIEWER_PASSWORD:-viewer-demo}
OP_COOKIE="${TMPDIR:-/tmp}/control-dashboard-step46-op-cookies.$$"
VIEW_COOKIE="${TMPDIR:-/tmp}/control-dashboard-step46-view-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step46-body.$$"
PREVIEW_BODY="${TMPDIR:-/tmp}/control-dashboard-step46-preview.$$"
ROUTE_BODY="${TMPDIR:-/tmp}/control-dashboard-step46-route.$$"
trap 'rm -f "$OP_COOKIE" "$VIEW_COOKIE" "$BODY" "$PREVIEW_BODY" "$ROUTE_BODY"' EXIT

fail() { echo "STEP46: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP46: PASS: $*"; }
info() { echo "STEP46: INFO: $*"; }

login_json() {
  python3 - "$1" "$2" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

login() {
  user=$1
  pass=$2
  jar=$3
  payload=$(login_json "$user" "$pass")
  code=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$jar" -H 'Content-Type: application/json' --data "$payload" "$BASE_URL/api/auth/login" 2>/dev/null || true)
  [ "$code" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "login for $user failed with HTTP $code"; }
  python3 - "$BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
t=d.get('csrfToken')
if not isinstance(t,str) or not t:
    raise SystemExit(1)
print(t)
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 46 — MANUAL CONTROL SAFE ROUTING CONTRACT"
echo "============================================================"
echo "Confirmed/hash-bound/stale-reference-checked OPERATOR admission with"
echo "durable intent audit. Actual trading remains fail-closed: NO wallet,"
echo "private signing, exchange submit/cancel, NATS trading publish or capital movement."
echo

echo "[1/10] Step 45 Live vs Expected precondition"
"$ROOT/scripts/step45-live-vs-expected.sh" >/dev/null || fail "Step 45 Live vs Expected gate failed"
pass "Step 45 remains valid; private Steps 37-41 remain deferred"

echo "[2/10] Go regression + Step 46 manual-audit/admission tests"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
elif command -v docker >/dev/null 2>&1; then
  STEP46_GO_IMAGE=${STEP46_GO_IMAGE:-control-dashboard-step46-go:local}
  docker build --target build -t "$STEP46_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null || fail "could not build local Go regression image"
  docker run --rm "$STEP46_GO_IMAGE" sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' || fail "Go test/vet/build failed inside local build-stage image"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi
pass "manual audit, omitted-current-asset, execution-constraint and route-admission regressions pass"

echo "[3/10] Authenticated Step 46 manual-control contract"
OP_CSRF=$(login "$OPERATOR_USER" "$OPERATOR_PASS" "$OP_COOKIE") || fail "operator login/CSRF failed"
ATTEMPT=1
while :; do
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" "$BASE_URL/api/manual-control" 2>/dev/null || true)
  if [ "$CODE" = "200" ] && python3 - "$BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
assert d.get('contractVersion') == 'step46-v1'
assert d.get('routingContractReady') is True
assert d.get('humanAuditAvailable') is True
assert d.get('routeEnabled') is False
assert d.get('riskCheckAvailable') is False
assert d.get('routingContractMode') == 'ADMISSION_ONLY_FAIL_CLOSED'
assert d.get('tradingControlSink') == 'UNCONFIGURED'
assert d.get('privateAuth') == 'DEFERRED'
assert d.get('orderLifecycle') == 'DEFERRED'
assert d.get('confirmationRequired') is True
assert d.get('confirmationPhrase') == 'CONFIRM_MANUAL_ROUTE'
assert d.get('sourceMode') == 'REAL'
PY
  then
    break
  fi
  if [ "$ATTEMPT" -ge 6 ]; then
    cat "$BODY" >&2 2>/dev/null || true
    fail "Step 46 manual-control contract did not become readable/ready"
  fi
  info "manual-control contract not ready yet; retrying in 2s ($ATTEMPT/6)..."
  ATTEMPT=$((ATTEMPT + 1))
  sleep 2
done
pass "contract=step46-v1 durableAudit=AVAILABLE routing=DISABLED privateAuth=DEFERRED"

echo "[4/10] Route admission authentication, role and CSRF boundary"
UNAUTH_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -H 'Content-Type: application/json' --data '{"confirmation":"CONFIRM_MANUAL_ROUTE"}' "$BASE_URL/api/manual-control/route" 2>/dev/null || true)
[ "$UNAUTH_CODE" = "401" ] || fail "unauthenticated route admission returned HTTP $UNAUTH_CODE; expected 401"
VIEW_CSRF=$(login "$VIEWER_USER" "$VIEWER_PASS" "$VIEW_COOKIE") || fail "viewer login/CSRF failed"
VIEW_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$VIEW_COOKIE" -H "X-CSRF-Token: $VIEW_CSRF" -H 'Content-Type: application/json' --data '{"confirmation":"CONFIRM_MANUAL_ROUTE"}' "$BASE_URL/api/manual-control/route" 2>/dev/null || true)
[ "$VIEW_CODE" = "403" ] || fail "VIEWER route admission returned HTTP $VIEW_CODE; expected 403"
NO_CSRF_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H 'Content-Type: application/json' --data '{"confirmation":"CONFIRM_MANUAL_ROUTE"}' "$BASE_URL/api/manual-control/route" 2>/dev/null || true)
[ "$NO_CSRF_CODE" = "403" ] || fail "OPERATOR route admission without CSRF returned HTTP $NO_CSRF_CODE; expected 403"
BAD_CONFIRM_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H "X-CSRF-Token: $OP_CSRF" -H 'Content-Type: application/json' --data '{"confirmation":"NO"}' "$BASE_URL/api/manual-control/route" 2>/dev/null || true)
[ "$BAD_CONFIRM_CODE" = "400" ] || fail "invalid confirmation returned HTTP $BAD_CONFIRM_CODE; expected 400"
pass "route admission requires authenticated OPERATOR + CSRF + explicit confirmation"

echo "[5/10] Server preview binds exact CSV hash + current target reference"
# Re-read the base model and use its server-generated example CSV so this gate
# never invents a portfolio. The preview may be execution-blocked if the current
# target contains non-routable assets; Step 46 still must hash it deterministically.
curl -fsS -b "$OP_COOKIE" "$BASE_URL/api/manual-control" >"$BODY" || fail "manual-control base read failed"
python3 - "$BODY" >"$PREVIEW_BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
print(json.dumps({"filename":"step46-gate.csv","csv":d.get("exampleCsv","")}, ensure_ascii=False))
PY
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H "X-CSRF-Token: $OP_CSRF" -H 'Content-Type: application/json' --data-binary @"$PREVIEW_BODY" "$BASE_URL/api/manual-control/preview" 2>/dev/null || true)
[ "$CODE" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "manual preview returned HTTP $CODE"; }
python3 - "$BODY" <<'PY' || exit 1
import json, re, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('contractVersion') != 'step46-v1': raise SystemExit('STEP46: FAIL: preview contractVersion mismatch')
if d.get('routeEnabled') is not False or d.get('riskCheckAvailable') is not False: raise SystemExit('STEP46: FAIL: preview implies routing/risk approval')
h=d.get('requestHash','')
if not re.fullmatch(r'sha256:[0-9a-f]{64}', h): raise SystemExit('STEP46: FAIL: preview requestHash is not canonical SHA-256')
if not str(d.get('currentTargetTimestamp') or '').strip(): raise SystemExit('STEP46: FAIL: preview lacks currentTargetTimestamp')
if d.get('confirmationPhrase') != 'CONFIRM_MANUAL_ROUTE': raise SystemExit('STEP46: FAIL: preview confirmation phrase mismatch')
print(f"STEP46: PASS: preview hash={h[:23]}… reference={d.get('currentTargetTimestamp')} validation={d.get('validationPassed')} exchangeConstraints={d.get('exchangeConstraintsValidated')}")
PY

# Build the route request from the exact server preview and exact CSV payload.
python3 - "$PREVIEW_BODY" "$BODY" >"$ROUTE_BODY" <<'PY'
import json, sys
request=json.load(open(sys.argv[1], encoding='utf-8'))
preview=json.load(open(sys.argv[2], encoding='utf-8'))
print(json.dumps({
  "filename": request["filename"],
  "csv": request["csv"],
  "requestHash": preview["requestHash"],
  "referenceTargetTimestamp": preview["currentTargetTimestamp"],
  "confirmation": "CONFIRM_MANUAL_ROUTE",
}, ensure_ascii=False))
PY

echo "[6/10] Confirmed route admission persists intent but submits nothing"
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H "X-CSRF-Token: $OP_CSRF" -H 'Content-Type: application/json' --data-binary @"$ROUTE_BODY" "$BASE_URL/api/manual-control/route" 2>/dev/null || true)
[ "$CODE" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "confirmed route admission returned HTTP $CODE"; }
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('contractVersion') != 'step46-v1' or d.get('status') != 'BLOCKED': raise SystemExit('STEP46: FAIL: route admission is not step46-v1/BLOCKED')
if d.get('submitted') is not False or d.get('routeEnabled') is not False: raise SystemExit('STEP46: FAIL: route admission submitted/enabled routing')
if d.get('confirmationAccepted') is not True or d.get('auditPersisted') is not True: raise SystemExit('STEP46: FAIL: confirmation/audit contract not satisfied')
if not str(d.get('correlationId') or '').startswith('manual-route-'): raise SystemExit('STEP46: FAIL: missing manual-route correlation id')
required={'MANUAL_RISK_CONTRACT_UNAVAILABLE','TRADING_CONTROL_SINK_UNCONFIGURED','PRIVATE_AUTH_DEFERRED','ORDER_LIFECYCLE_DEFERRED','GLOBAL_TRADING_READINESS_FALSE'}
missing=sorted(required-set(d.get('blockers') or []))
if missing: raise SystemExit('STEP46: FAIL: missing fail-closed blockers: '+', '.join(missing))
print(f"STEP46: PASS: admission={d.get('status')} submitted=false audit=PERSISTED correlation={d.get('correlationId')}")
PY

# Hash tampering must still be safely rejected and durably audited.
python3 - "$ROUTE_BODY" >"$PREVIEW_BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
d['requestHash']='sha256:'+'0'*64
print(json.dumps(d, ensure_ascii=False))
PY
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H "X-CSRF-Token: $OP_CSRF" -H 'Content-Type: application/json' --data-binary @"$PREVIEW_BODY" "$BASE_URL/api/manual-control/route" 2>/dev/null || true)
[ "$CODE" = "200" ] || fail "tampered-hash admission returned HTTP $CODE; expected safe BLOCKED response"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('submitted') is not False or d.get('auditPersisted') is not True or 'REQUEST_HASH_MISMATCH' not in (d.get('blockers') or []):
    raise SystemExit('STEP46: FAIL: tampered request hash was not durably rejected')
print('STEP46: PASS: tampered request hash is rejected, not submitted, and audited')
PY

echo "[7/10] Dedicated append-only operator-intent store + Alerts/Audit projection"
command -v docker >/dev/null 2>&1 || fail "Docker is required to inspect the isolated manual-audit volume"
docker exec control-dashboard-api sh -ec 'test -s /data/manual-audit/events.jsonl && tail -n 10 /data/manual-audit/events.jsonl' >"$BODY" || fail "manual audit events.jsonl is not persisted in dashboard-api volume"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
rows=[json.loads(line) for line in open(sys.argv[1], encoding='utf-8') if line.strip()]
if not rows: raise SystemExit('STEP46: FAIL: manual audit store is empty')
latest=rows[-1]
if latest.get('version') != 'step46-v1' or latest.get('contractVersion') != 'step46-v1': raise SystemExit('STEP46: FAIL: manual audit store version mismatch')
if latest.get('action') != 'MANUAL_ROUTE_ADMISSION' or latest.get('submitted') is not False: raise SystemExit('STEP46: FAIL: manual audit latest event is not a non-submitted route admission')
if not latest.get('requestHash') or not latest.get('correlationId'): raise SystemExit('STEP46: FAIL: manual audit event lacks hash/correlation')
print(f"STEP46: PASS: durable manual audit event={latest.get('eventId')} result={latest.get('result')} submitted=false")
PY

ATTEMPT=1
while :; do
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" "$BASE_URL/api/alerts-audit" 2>/dev/null || true)
  if [ "$CODE" = "200" ] && python3 - "$BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
assert d.get('humanAuditAvailable') is True
assert 'DURABLE_MANUAL_INTENT' in str(d.get('auditMode') or '')
assert any(row.get('action') == 'MANUAL_ROUTE_ADMISSION' and row.get('actorType') == 'HUMAN' for row in d.get('audit',[]))
PY
  then break; fi
  [ "$ATTEMPT" -lt 5 ] || { cat "$BODY" >&2 2>/dev/null || true; fail "Alerts & Audit did not expose durable manual intent"; }
  ATTEMPT=$((ATTEMPT + 1)); sleep 1
done
pass "Alerts & Audit exposes durable HUMAN manual-route intent; alert acknowledgement remains deferred"

echo "[8/10] Global readiness marks dashboard phase complete without enabling trading"
ATTEMPT=1
while :; do
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" "$BASE_URL/api/global-readiness" 2>/dev/null || true)
  if [ "$CODE" = "200" ] && python3 - "$BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
rows={r.get('id'):r for r in d.get('requirements',[])}
assert d.get('status') == 'VALIDATED_FAIL_CLOSED'
assert d.get('safeToContinueDashboard') is True
assert d.get('currentPhase') == 'DASHBOARD_COMPLETE_PRIVATE_TESTNET_DEFERRED'
assert d.get('nextSafeStep') == 'STEP_37_PRIVATE_TESTNET_AUTH'
assert d.get('manualRouting') == 'DISABLED'
assert d.get('privateTestnetReady') is False and d.get('tradingReady') is False and d.get('liveReady') is False
assert rows.get('manual-route',{}).get('state') == 'PASS'
PY
  then break; fi
  [ "$ATTEMPT" -lt 6 ] || { cat "$BODY" >&2 2>/dev/null || true; fail "global readiness did not reach dashboard-complete fail-closed phase"; }
  info "global readiness has transient blocker(s); retrying in 2s ($ATTEMPT/6)..."
  ATTEMPT=$((ATTEMPT + 1)); sleep 2
done
pass "dashboard phase is complete; private TESTNET/trading/LIVE remain independently false"

echo "[9/10] Static command-surface and persistence isolation boundary"
SERVER_GO="$ROOT/dashboard-api/internal/server/server.go"
grep -q 'POST /api/manual-control/route' "$SERVER_GO" || fail "Step 46 route-admission endpoint is missing"
grep -q 'Submitted: false' "$SERVER_GO" || fail "route-admission response no longer pins submitted=false"
grep -q 'RouteEnabled: false' "$SERVER_GO" || fail "route-admission response no longer pins routeEnabled=false"
if grep -nE 'mux\.Handle(Func)?\("(GET|POST|PUT|PATCH|DELETE) /api/(submit|cancel|order|orders|trade|exchange)(/|"| )' "$SERVER_GO" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "capital-moving/trading command API route is registered"
fi
if grep -RInE '\.Publish\(|Publish\(' "$ROOT/dashboard-api/internal/manualaudit" "$ROOT/dashboard-api/internal/provider/manual_control.go" "$SERVER_GO" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "manual-control admission contains a publish call"
fi
if grep -RInE 'PRIVATE_KEY|MNEMONIC|SEED_PHRASE|HYPERLIQUID_[A-Z0-9_]*SECRET|API_WALLET_[A-Z0-9_]*KEY' "$ROOT/dashboard-api/cmd" "$ROOT/dashboard-api/internal" --include='*.go' >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "secret-bearing runtime identifier detected in dashboard Go source"
fi
grep -q 'dashboard-manual-audit-data:/data/manual-audit' "$ROOT/docker-compose.real.yml" || fail "API manual-audit named volume mount missing"
if grep -A25 'dashboard-watchdog:' "$ROOT/docker-compose.real.yml" | grep -q '/data/manual-audit'; then
  fail "watchdog must not write the manual operator-intent audit volume"
fi
pass "manual admission writes only its dedicated audit store; no publish/exchange/signing/order surface exists"

echo "[10/10] Manual Control + Alerts/Audit UI exposes Step 46 evidence"
grep -q 'Step 46 · Routing Admission Contract' "$ROOT/src/pages/ManualControlPage.tsx" || fail "Manual Control Step 46 contract panel missing"
grep -q 'Evaluate safe route admission' "$ROOT/src/pages/ManualControlPage.tsx" || fail "Manual Control admission action missing"
grep -q 'Durable Operator-Intent Audit' "$ROOT/src/pages/ManualControlPage.tsx" || fail "Manual Control durable audit panel missing"
grep -q 'routeManualControl' "$ROOT/src/data/dashboardDataSource.ts" || fail "frontend route-admission data-source contract missing"
grep -q 'durable manual intent' "$ROOT/src/pages/AlertsAuditPage.tsx" || fail "Alerts/Audit durable manual-intent UI label missing"
pass "operator UI exposes confirmation/admission/audit evidence without implying execution"

echo
echo "============================================================"
echo "STEP 46: PASS — MANUAL CONTROL SAFE ROUTING CONTRACT VALIDATED"
echo "============================================================"
echo "The dashboard phase is functionally complete. Manual control now has a"
echo "versioned server-side confirmation/hash/stale-reference admission contract"
echo "and isolated durable OPERATOR intent audit, but it still submits NOTHING."
echo "PortfolioRisk manual transformation, trading-control sink, Hyperliquid private"
echo "auth and order lifecycle remain DEFERRED; routing remains DISABLED."
echo "Next safe phase: resume Step 37 Hyperliquid private TESTNET authentication."
