#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
PROJECT_ROOT=${ALGOTRADING_PROJECT_ROOT:-$(CDPATH= cd -- "$ROOT/.." && pwd)}
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step33-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step33-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP33: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP33: PASS: $*"; }
info() { echo "STEP33: INFO: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 33 — TESTNET INTEGRATION FOUNDATION"
echo "============================================================"
echo "Identity/config foundation only: NO venue network calls, secrets, signing or orders."
echo

echo "[1/8] Step 32 safety precondition"
LOGIN_JSON=$(json_login_body)

# docker compose can report dashboard-api healthy and dashboard-web started a
# few seconds before Caddy is ready to accept the first localhost connection.
# Retry ONLY transport/unready HTTP states here; once the endpoint answers with
# a real auth response, preserve fail-closed semantics.
LOGIN_ATTEMPTS=${STEP33_LOGIN_ATTEMPTS:-15}
LOGIN_RETRY_SECONDS=${STEP33_LOGIN_RETRY_SECONDS:-2}
attempt=1
while :; do
  : >"$BODY"
  if CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" \
      -H 'Content-Type: application/json' --data "$LOGIN_JSON" \
      "$BASE_URL/api/auth/login"); then
    if [ "$CODE" = "200" ]; then
      pass "dashboard login endpoint is reachable"
      break
    fi

    # 502/503/504 can occur while the reverse proxy/upstream is settling.
    # Authentication/configuration failures are not retryable.
    case "$CODE" in
      502|503|504)
        ;;
      *)
        cat "$BODY" >&2 || true
        fail "dashboard login returned HTTP $CODE"
        ;;
    esac
  else
    CODE=000
  fi

  if [ "$attempt" -ge "$LOGIN_ATTEMPTS" ]; then
    cat "$BODY" >&2 || true
    fail "dashboard login did not become reachable at $BASE_URL after $LOGIN_ATTEMPTS attempts (last HTTP $CODE)"
  fi
  info "waiting ${LOGIN_RETRY_SECONDS}s for dashboard web/API readiness (attempt $attempt/$LOGIN_ATTEMPTS; last HTTP $CODE)"
  sleep "$LOGIN_RETRY_SECONDS"
  attempt=$((attempt + 1))
done

# Immediately after dashboard-api recreation the canonical SQLite reader can
# briefly be unavailable while the live MarketData process/WAL sidecars settle.
# Retry ONLY that already-verified transient condition. Any other Step 32
# blocker still fails closed immediately.
SAFETY_ATTEMPTS=${STEP33_SAFETY_ATTEMPTS:-15}
SAFETY_RETRY_SECONDS=${STEP33_SAFETY_RETRY_SECONDS:-2}
attempt=1
while :; do
  GATE=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/safety-gate") || fail "Step 32 safety endpoint unavailable"
  printf '%s' "$GATE" >"$BODY"

  if python3 - "$BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
status=d.get('status')
safe=bool(d.get('safeToProceed'))
if status != 'BLOCKED' and safe:
    print(f"STEP33: PASS: Step 32 precondition status={status} safeToProceed={safe}")
    raise SystemExit(0)

blocked=[c for c in (d.get('checks') or []) if c.get('state') == 'BLOCKED']
retryable=(
    status == 'BLOCKED' and not safe and len(blocked) == 1 and
    blocked[0].get('id') == 'runtime' and
    'Canonical market-data diagnostics are unavailable' in str(blocked[0].get('detail',''))
)
if retryable:
    print('STEP33: INFO: Step 32 is temporarily blocked only by canonical market-data diagnostics.', file=sys.stderr)
    raise SystemExit(10)

print(json.dumps(d, indent=2), file=sys.stderr)
raise SystemExit(20)
PY
  then
    break
  else
    rc=$?
  fi

  if [ "$rc" -ne 10 ]; then
    fail "Step 32 safety precondition is not satisfied"
  fi
  if [ "$attempt" -ge "$SAFETY_ATTEMPTS" ]; then
    echo "STEP33: DIAGNOSTIC: canonical market-data diagnostics did not recover after $SAFETY_ATTEMPTS attempts" >&2
    for endpoint in source-status market-data; do
      CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" "$BASE_URL/api/$endpoint" || true)
      echo "STEP33: DIAGNOSTIC: GET /api/$endpoint -> HTTP $CODE" >&2
      cat "$BODY" >&2 || true
      echo >&2
    done
    fail "Step 32 safety precondition remains blocked by canonical market-data diagnostics"
  fi

  info "waiting ${SAFETY_RETRY_SECONDS}s for canonical market-data diagnostics (attempt $attempt/$SAFETY_ATTEMPTS)"
  sleep "$SAFETY_RETRY_SECONDS"
  attempt=$((attempt + 1))
done

echo "[2/8] Current ExchangeGateway dry-run source boundary"
GATEWAY="$PROJECT_ROOT/live_trading/exchange_gateway/src/exchange_gateway_main.cpp"
[ -f "$GATEWAY" ] || fail "ExchangeGateway source not found at $GATEWAY"
grep -q 'HyperliquidDryRun' "$GATEWAY" || fail "Hyperliquid dry-run mode is not present in current ExchangeGateway"
grep -q 'hyperliquid-dry-run' "$GATEWAY" || fail "hyperliquid-dry-run CLI mode is missing"
grep -q 'exchange-gateway-hyperliquid-dry-run-plan' "$GATEWAY" || fail "dry-run durable consumer is missing"
grep -q 'MessageSubjects::NOTIONAL_ORDER_PLAN' "$GATEWAY" || fail "dry-run does not consume the notional-plan boundary"
grep -q 'real_submission=false' "$GATEWAY" || fail "ExchangeGateway service-ready evidence no longer declares real_submission=false"
grep -q 'no backend and no real submission path' "$GATEWAY" || fail "dry-run no-submission safety marker is missing"
pass "current C++ gateway exposes the audited Hyperliquid prepare-only boundary"

echo "[3/8] Dashboard source-contract audit"
python3 "$ROOT/scripts/verify-project-contracts.py" --project-root "$PROJECT_ROOT" >/dev/null || fail "source contract audit failed"
pass "dashboard mappings still match audited trading contracts"

echo "[4/8] Go regression gate"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
  pass "Go test + vet + build"
elif command -v docker >/dev/null 2>&1; then
  STEP33_GO_IMAGE=${STEP33_GO_IMAGE:-control-dashboard-step33-go:local}
  docker build --target build -t "$STEP33_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null \
    || fail "could not build local Step 33 Go regression image"
  docker run --rm "$STEP33_GO_IMAGE" \
    sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' \
    || fail "Go regression gate failed inside local build-stage image"
  pass "Go test + vet + build (local Docker build-stage toolchain)"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi

echo "[5/8] No exchange secrets or trading command surface"
if grep -RInE 'DASHBOARD_(VENUE|EXCHANGE)_[A-Z0-9_]*(API_KEY|SECRET|PRIVATE_KEY|SIGNING_KEY)' \
  "$ROOT/docker-compose.yml" "$ROOT/docker-compose.real.yml" "$ROOT/docker-compose.production.yml" "$ROOT/docker-compose.production.real.yml" "$ROOT/dashboard-api" \
  --include='*.go' --include='*.yml' >"$BODY"; then
  cat "$BODY" >&2
  fail "exchange-secret configuration was introduced during the foundation step"
fi
ROUTES=$(sed -n '/func (s \*Server) routes()/,/return s.middleware/p' "$ROOT/dashboard-api/internal/server/server.go")
printf '%s\n' "$ROUTES" | grep -Eq '/api/(submit|cancel|pause|resume|kill|order)' && fail "dangerous trading route found"
pass "Step 33 contains no exchange secrets and no trading command HTTP route"

echo "[6/8] Venue foundation endpoint"
FOUNDATION=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/venue-foundation") || fail "venue-foundation endpoint unavailable"
printf '%s' "$FOUNDATION" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
expected={
    'venue':'HYPERLIQUID',
    'targetEnvironment':'TESTNET',
    'gatewayMode':'hyperliquid-dry-run',
    'publicConnectivity':'NOT_CHECKED',
    'privateAuth':'DISABLED',
    'orderRouting':'DISABLED',
    'symbolMapping':'NOT_CONFIGURED',
    'exchangeFilters':'NOT_LOADED',
}
for key,value in expected.items():
    if d.get(key) != value:
        raise SystemExit(f'STEP33: FAIL: {key}={d.get(key)!r}, want {value!r}')
if not d.get('foundationReady') or d.get('status') != 'READY_FOR_PUBLIC_CONNECTIVITY':
    raise SystemExit(f"STEP33: FAIL: foundation is not ready: {d.get('status')}")
if d.get('secretsRequired') or d.get('capitalRequired') or not d.get('readOnly'):
    raise SystemExit('STEP33: FAIL: foundation violates the no-secret/no-capital/read-only boundary')
print('STEP33: PASS: Hyperliquid TESTNET identity is explicit and remains read-only/no-secret/no-capital')
PY

echo "[7/8] Infrastructure exposes identity without claiming connectivity"
INFRA=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/infrastructure") || fail "infrastructure endpoint unavailable"
printf '%s' "$INFRA" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
e=d.get('exchange') or {}
if e.get('venue') != 'HYPERLIQUID':
    raise SystemExit(f"STEP33: FAIL: infrastructure venue={e.get('venue')!r}")
if e.get('connected') is True:
    raise SystemExit('STEP33: FAIL: Step 33 must not claim exchange connectivity')
if e.get('state') != 'UNKNOWN':
    raise SystemExit(f"STEP33: FAIL: exchange state must remain UNKNOWN until Step 34, got {e.get('state')!r}")
print('STEP33: PASS: Infrastructure names HYPERLIQUID but keeps connectivity UNKNOWN/fail-closed')
PY

echo "[8/8] Manual route remains disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP33: FAIL: manual-control routeEnabled=true')
print('STEP33: PASS: manual routing remains fail-closed')
PY

echo
echo "============================================================"
echo "STEP 33: PASS — TESTNET INTEGRATION FOUNDATION ESTABLISHED"
echo "============================================================"
echo "Next: Step 34 may add PUBLIC venue connectivity/metadata only."
echo "Private auth, signing, submit/cancel and real capital remain out of scope."
