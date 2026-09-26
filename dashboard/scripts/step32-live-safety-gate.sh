#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROJECT_ROOT=${ALGOTRADING_PROJECT_ROOT:-$(CDPATH= cd -- "$ROOT/.." && pwd)}
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step32-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step32-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP32: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP32: PASS: $*"; }
warn() { echo "STEP32: WARN: $*" >&2; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 32 — PRE-TESTNET SAFETY GATE"
echo "============================================================"
echo "This gate does NOT authorize live trading or capital movement."
echo

echo "[1/9] Source-contract audit"
python3 "$ROOT/scripts/verify-project-contracts.py" --project-root "$PROJECT_ROOT" >/dev/null || fail "source contract audit failed"
pass "audited trading contracts still match the dashboard mapping"

echo "[2/9] Go regression gate"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
  pass "Go test + vet + build (CGO_ENABLED=0, matching production binary)"
elif command -v docker >/dev/null 2>&1; then
  # Do not `docker run golang:...` directly here. On some Docker Desktop/WSL
  # installations the CLI credential helper can fail on an implicit pull even
  # though BuildKit/Compose can already build the dashboard successfully.
  # Build the existing Dockerfile's Go stage into a local image, then execute
  # the regression checks from that image. This reuses the exact dashboard API
  # build context and does not require Go to be installed in WSL.
  STEP32_GO_IMAGE=${STEP32_GO_IMAGE:-control-dashboard-step32-go:local}
  docker build \
    --target build \
    -t "$STEP32_GO_IMAGE" \
    "$ROOT/dashboard-api" >/dev/null \
    || fail "could not build the local Go regression image"
  docker run --rm \
    "$STEP32_GO_IMAGE" \
    sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' \
    || fail "Go test/vet/build failed inside the local build-stage image"
  pass "Go test + vet + build (CGO_ENABLED=0, local Docker build-stage toolchain)"
else
  fail "neither Go nor Docker is available to run the Go regression gate"
fi

echo "[3/9] NATS bridge remains subscription-only"
# JetStream direct/management reads use request/reply frames internally; those are
# allowed read operations. The event bridge itself must remain SUB-only and the
# provider must not gain a trading publish API.
if grep -nE '(^|[^A-Z])(PUB|HPUB)[[:space:]]|\.Publish\(|PublishMsg' "$ROOT/dashboard-api/internal/integration/nats/subscriber.go" >"$BODY"; then
  cat "$BODY" >&2
  fail "NATS SSE invalidation bridge contains a publish operation"
fi
if grep -RInE '\.Publish\(|PublishMsg' "$ROOT/dashboard-api/internal/provider" --include='*.go' --exclude='*_test.go' >"$BODY"; then
  cat "$BODY" >&2
  fail "provider contains a NATS publish API"
fi
pass "SSE bridge is subscription-only; no provider trading publish API found"

echo "[4/9] No trading command HTTP surface"
ROUTES=$(sed -n '/func (s \*Server) routes()/,/return s.middleware/p' "$ROOT/dashboard-api/internal/server/server.go")
printf '%s\n' "$ROUTES" | grep -Eq '/api/(submit|cancel|pause|resume|kill|order)' && fail "dangerous trading route found"
printf '%s\n' "$ROUTES" | grep -q 'POST /api/manual-control/preview' || fail "manual preview safety endpoint missing"
printf '%s\n' "$ROUTES" | grep -q 'POST /api/manual-control/route' || fail "Step 46 manual route-admission endpoint missing"
grep -q 'Submitted: false' "$ROOT/dashboard-api/internal/server/server.go" || fail "Step 46 route admission no longer pins submitted=false"
grep -q 'RouteEnabled: false' "$ROOT/dashboard-api/internal/server/server.go" || fail "Step 46 route admission no longer pins routeEnabled=false"
pass "no submit/cancel/pause/resume/kill/order route; Step 46 manual admission is explicitly non-submitting"

echo "[5/9] PostgreSQL adapter remains read-only"
if grep -RInE '\b(INSERT|UPDATE|DELETE|ALTER|DROP|TRUNCATE|CREATE[[:space:]]+TABLE)\b' "$ROOT/dashboard-api/internal/integration/postgres" --include='*.go' --exclude='*_test.go' >"$BODY"; then
  cat "$BODY" >&2
  fail "mutating SQL found in dashboard PostgreSQL adapter"
fi
pass "no mutating SQL in PostgreSQL integration"

echo "[6/9] Production hardening boundary"
grep -q 'DASHBOARD_AUTH_ALLOW_DEMO: "false"' "$ROOT/docker-compose.production.yml" || fail "production demo auth is not disabled"
grep -q 'DASHBOARD_AUTH_COOKIE_SECURE: "true"' "$ROOT/docker-compose.production.yml" || fail "production Secure cookie is not forced"
grep -q 'internal: true' "$ROOT/docker-compose.production.yml" || fail "production private network is not internal"
grep -q 'no-new-privileges:true' "$ROOT/docker-compose.production.yml" || fail "no-new-privileges missing"
grep -q 'read_only: true' "$ROOT/docker-compose.production.yml" || fail "read-only root filesystem missing"
pass "production auth/network/container hardening is present"

echo "[7/9] Dashboard login + safety endpoint"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login") || fail "cannot reach dashboard login at $BASE_URL"
[ "$CODE" = "200" ] || { cat "$BODY" >&2; fail "dashboard login returned HTTP $CODE"; }
GATE=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/safety-gate") || fail "safety-gate endpoint unavailable"
printf '%s' "$GATE" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
p=sys.argv[1]
d=json.load(open(p, encoding='utf-8'))
if d.get('status') == 'BLOCKED' or not d.get('safeToProceed'):
    print(json.dumps(d, indent=2), file=sys.stderr)
    runtime = next((c for c in d.get('checks', []) if c.get('id') == 'runtime'), None)
    if runtime:
        print('STEP32: BLOCKER DETAIL: ' + str(runtime.get('detail', 'unknown runtime blocker')), file=sys.stderr)
    raise SystemExit('STEP32: FAIL: runtime safety gate is BLOCKED')
print(f"STEP32: PASS: runtime safety status={d.get('status')} safeToProceed={d.get('safeToProceed')}")
print(f"STEP32: INFO: tradingReady={d.get('tradingReady')} runtimeReadiness={d.get('runtimeReadiness')} proof={d.get('endToEndProofStatus')}/{d.get('endToEndCompletion')}")
PY

echo "[8/9] Manual route is still fail-closed"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP32: FAIL: manual-control routeEnabled=true')
print('STEP32: PASS: manual-control routeEnabled=false')
PY

echo "[9/9] Step 31 proof has no contradiction"
PIPE=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/pipeline") || fail "pipeline read model unavailable"
printf '%s' "$PIPE" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
p=d.get('proof') or {}
status=p.get('status','UNKNOWN')
completion=p.get('completion','UNKNOWN')
if status == 'BLOCKED':
    raise SystemExit(f'STEP32: FAIL: Step 31 proof is BLOCKED ({completion})')
print(f'STEP32: PASS: Step 31 proof is not contradictory ({status}/{completion})')
PY

echo
echo "============================================================"
echo "STEP 32: PASS — SAFE TO CONTINUE TOWARD TESTNET INTEGRATION"
echo "============================================================"
echo "This is NOT TRADING READY. A WARN/DEGRADED shell is expected until"
echo "exchange connectivity, common service liveness/control state and host"
echo "clock-sync contracts are independently observable."
