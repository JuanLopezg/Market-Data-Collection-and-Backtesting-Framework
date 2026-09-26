#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step45-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step45-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP45: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP45: PASS: $*"; }
info() { echo "STEP45: INFO: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 45 — LIVE VS EXPECTED REAL PROJECTION"
echo "============================================================"
echo "Versioned/fingerprinted read-only anomaly projection over canonical market history."
echo "NO expected-PnL claim, wallet, private auth, signing, submit/cancel or capital movement."
echo

echo "[1/8] Step 44 global readiness precondition"
"$ROOT/scripts/step44-global-readiness.sh" >/dev/null || fail "Step 44 Full Global Readiness gate failed"
pass "Step 44 remains valid; private Steps 37-41 stay deferred"

echo "[2/8] Go regression + Step 45 projection tests"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
elif command -v docker >/dev/null 2>&1; then
  STEP45_GO_IMAGE=${STEP45_GO_IMAGE:-control-dashboard-step45-go:local}
  docker build --target build -t "$STEP45_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null || fail "could not build local Go regression image"
  docker run --rm "$STEP45_GO_IMAGE" sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' || fail "Go test/vet/build failed inside local build-stage image"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi
pass "projection contract, classification, fingerprint and full Go regression pass"

echo "[3/8] Authenticated REAL projection read model"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login" 2>/dev/null || true)
[ "$CODE" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "dashboard login failed with HTTP $CODE"; }

ATTEMPT=1
MAX_ATTEMPTS=${STEP45_READ_ATTEMPTS:-6}
while :; do
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" "$BASE_URL/api/live-vs-expected" 2>/dev/null || true)
  if [ "$CODE" = "200" ]; then
    break
  fi
  if [ "$ATTEMPT" -ge "$MAX_ATTEMPTS" ]; then
    cat "$BODY" >&2 2>/dev/null || true
    fail "live-vs-expected endpoint did not become readable after $MAX_ATTEMPTS attempts (HTTP $CODE)"
  fi
  case "$CODE" in
    000|502|503|504)
      info "bounded read/transport blocker HTTP $CODE; retrying in 2s ($ATTEMPT/$MAX_ATTEMPTS)..."
      ATTEMPT=$((ATTEMPT + 1))
      sleep 2
      ;;
    *)
      cat "$BODY" >&2 2>/dev/null || true
      fail "live-vs-expected endpoint returned non-retryable HTTP $CODE"
      ;;
  esac
done

python3 - "$BODY" <<'PY' || exit 1
import json, math, re, sys
p=sys.argv[1]
d=json.load(open(p, encoding='utf-8'))
if d.get('contractVersion') != 'step45-v1':
    raise SystemExit('STEP45: FAIL: contractVersion is not step45-v1')
if d.get('status') != 'VALIDATED_LIMITED' or d.get('validated') is not True:
    raise SystemExit('STEP45: FAIL: REAL projection contract is not VALIDATED_LIMITED/validated')
if d.get('sourceMode') != 'REAL':
    raise SystemExit('STEP45: FAIL: projection is not backed by the REAL provider')
if d.get('observationMode') != 'LATEST_COMPLETED_CANONICAL_MARKET_DAY':
    raise SystemExit('STEP45: FAIL: observation basis is not explicit/latest completed canonical day')
fp=d.get('baselineFingerprint','')
if not re.fullmatch(r'[0-9a-f]{64}', fp):
    raise SystemExit('STEP45: FAIL: baseline fingerprint is not a full SHA-256 hex digest')
if d.get('baselineExcludesLatest') is not True:
    raise SystemExit('STEP45: FAIL: baseline does not explicitly exclude the latest observation')
if d.get('readOnly') is not True or d.get('orderRouting') != 'DISABLED' or d.get('privateAuth') not in ('DEFERRED','DISABLED'):
    raise SystemExit('STEP45: FAIL: projection safety boundary changed')
metrics=d.get('metrics') or []
anomalies=d.get('anomalies') or []
if d.get('projectionReady') is True:
    if int(d.get('baselineObservationCount') or 0) < 5:
        raise SystemExit('STEP45: FAIL: ready projection has fewer than five independent baseline observations')
    if len(metrics) != int(d.get('metricCount') or 0) or len(metrics) != 5:
        raise SystemExit(f'STEP45: FAIL: expected five canonical market-input metrics, got {len(metrics)}')
    counts=sum(int(d.get(k) or 0) for k in ('normalCount','elevatedCount','abnormalCount','criticalCount'))
    if counts != len(metrics):
        raise SystemExit('STEP45: FAIL: classification counters do not cover every metric exactly once')
    for m in metrics:
        if m.get('classification') not in ('NORMAL','ELEVATED','ABNORMAL','CRITICAL'):
            raise SystemExit('STEP45: FAIL: metric has invalid classification')
        z=m.get('zScore')
        if not isinstance(z,(int,float)) or not math.isfinite(z):
            raise SystemExit('STEP45: FAIL: metric has non-finite z-score')
        if not m.get('trend') or len(m.get('distribution') or []) != 15:
            raise SystemExit('STEP45: FAIL: metric trend/distribution evidence is incomplete')
    non_normal=sum(1 for m in metrics if m.get('classification') != 'NORMAL')
    if int(d.get('anomalyCount') or 0) != len(anomalies) or len(anomalies) != non_normal:
        raise SystemExit('STEP45: FAIL: anomaly projection does not match non-normal metric classifications')
    if d.get('overallClassification') not in ('NORMAL','ELEVATED','ABNORMAL','CRITICAL'):
        raise SystemExit('STEP45: FAIL: overall classification is invalid')
    print(f"STEP45: PASS: contract=step45-v1 projection=READY metrics={len(metrics)} anomalies={len(anomalies)} overall={d.get('overallClassification')} baselineObs={d.get('baselineObservationCount')} fingerprint={fp[:16]}…")
else:
    if d.get('overallClassification') != 'INSUFFICIENT_DATA':
        raise SystemExit('STEP45: FAIL: projectionReady=false without explicit INSUFFICIENT_DATA classification')
    if int(d.get('baselineObservationCount') or 0) >= 5:
        raise SystemExit('STEP45: FAIL: projection is unready despite having enough baseline observations')
    if int(d.get('metricCount') or 0) != 0 or metrics or int(d.get('anomalyCount') or 0) != 0 or anomalies:
        raise SystemExit('STEP45: FAIL: insufficient-history state fabricated metrics or anomalies')
    print(f"STEP45: PASS: contract=step45-v1 projection=INSUFFICIENT_DATA baselineObs={d.get('baselineObservationCount')} fingerprint={fp[:16]}…; no classification fabricated")
PY

echo "[4/8] Coverage is honest: market inputs REAL, unsupported baseline families DEFERRED"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
rows={r.get('id'):r for r in d.get('coverage',[])}
expected={'market-inputs','execution-distribution','performance-accounting','accepted-replay'}
missing=sorted(expected-set(rows))
if missing:
    raise SystemExit('STEP45: FAIL: missing coverage IDs: '+', '.join(missing))
market_state=rows['market-inputs'].get('state')
if d.get('projectionReady') is True:
    if market_state != 'VALIDATED':
        raise SystemExit('STEP45: FAIL: ready projection requires VALIDATED canonical market-input coverage')
else:
    if market_state != 'INSUFFICIENT_DATA' or d.get('overallClassification') != 'INSUFFICIENT_DATA':
        raise SystemExit('STEP45: FAIL: unready projection must disclose INSUFFICIENT_DATA market coverage')
for key in ('execution-distribution','performance-accounting','accepted-replay'):
    if rows[key].get('state') != 'DEFERRED':
        raise SystemExit(f'STEP45: FAIL: {key} must remain explicit DEFERRED')
note=(d.get('sourceNote') or '').lower()
for term in ('pnl','slippage','replay'):
    if term not in note:
        raise SystemExit(f'STEP45: FAIL: sourceNote does not disclose deferred {term} baseline')
print(f"STEP45: PASS: market-input coverage={market_state}; no execution/PnL/replay baseline is fabricated")
PY

echo "[5/8] Live-vs-expected endpoint is authenticated GET-only"
UNAUTH_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' "$BASE_URL/api/live-vs-expected" 2>/dev/null || true)
[ "$UNAUTH_CODE" = "401" ] || fail "unauthenticated GET /api/live-vs-expected returned HTTP $UNAUTH_CODE; expected 401"
POST_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" -X POST "$BASE_URL/api/live-vs-expected" 2>/dev/null || true)
[ "$POST_CODE" = "405" ] || fail "POST /api/live-vs-expected returned HTTP $POST_CODE; expected 405"
pass "projection endpoint remains authenticated and read-only"

echo "[6/8] Live vs Expected UI exposes Step 45 contract + explicit coverage"
grep -q 'Step 45 · Real Projection Contract' "$ROOT/src/pages/LiveVsExpectedPage.tsx" || fail "Step 45 projection panel missing from Live vs Expected page"
grep -q 'baselineFingerprint' "$ROOT/src/pages/LiveVsExpectedPage.tsx" || fail "baseline fingerprint is not visible"
grep -q 'Current Anomaly Projection' "$ROOT/src/pages/LiveVsExpectedPage.tsx" || fail "anomaly projection is not visible"
grep -q 'accepted-replay' "$ROOT/dashboard-api/internal/provider/real_live_vs_expected.go" || fail "accepted replay coverage boundary missing"
pass "UI exposes versioned identity, anomalies and deferred baseline families"

echo "[7/8] No trading authorization is inferred from anomaly classification"
grep -q 'Classifications are observational and do not enable, disable or recommend trades' "$ROOT/dashboard-api/internal/provider/real_live_vs_expected.go" || fail "observability-only policy note missing"
SERVER_GO="$ROOT/dashboard-api/internal/server/server.go"
if grep -nE 'mux\.Handle(Func)?\("(GET|POST|PUT|PATCH|DELETE) /api/(submit|cancel|order|orders|trade|exchange)(/|"| )' "$SERVER_GO" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "trading command API route is registered"
fi
pass "anomaly projection has no order/control command path"

echo "[8/8] Manual route remains disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP45: FAIL: manual-control routeEnabled=true')
print('STEP45: PASS: manual routing remains fail-closed')
PY

echo
echo "============================================================"
echo "STEP 45: PASS — LIVE VS EXPECTED REAL PROJECTION VALIDATED"
echo "============================================================"
echo "The real dashboard now exposes a validated versioned/fingerprinted projection contract"
echo "for canonical market/strategy-input behaviour. Classification remains fail-closed when"
echo "canonical history is insufficient. It does NOT claim expected PnL"
echo "or unavailable execution/replay distributions, and no anomaly enables routing."
echo "Step 37 private auth and Steps 38-41 remain DEFERRED; no wallet/funds required."
echo "Next safe step: Step 46 Manual Control Safe Routing contract (still fail-closed without private venue auth)."
