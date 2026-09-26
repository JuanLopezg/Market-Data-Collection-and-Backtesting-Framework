#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step44-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step44-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP44: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP44: PASS: $*"; }
info() { echo "STEP44: INFO: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 44 — FULL GLOBAL READINESS CONTRACT"
echo "============================================================"
echo "Explicit phase-scoped readiness: dashboard continuation can be safe while"
echo "private TESTNET, trading and LIVE remain independently fail-closed."
echo "NO wallet, private auth, signing, submit/cancel or capital movement."
echo

echo "[1/8] Step 43 durable watchdog precondition"
"$ROOT/scripts/step43-alert-watchdog.sh" >/dev/null || fail "Step 43 durable Alerts / Watchdog gate failed"
pass "Step 43 remains valid; private Steps 37-41 stay deferred"

echo "[2/8] Go regression + Step 44 readiness unit tests"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
elif command -v docker >/dev/null 2>&1; then
  STEP44_GO_IMAGE=${STEP44_GO_IMAGE:-control-dashboard-step44-go:local}
  docker build --target build -t "$STEP44_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null || fail "could not build local Go regression image"
  docker run --rm "$STEP44_GO_IMAGE" sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' || fail "Go test/vet/build failed inside local build-stage image"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi
pass "readiness builder fail-closed tests and full Go regression pass"

echo "[3/8] Authenticated global readiness contract"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login" 2>/dev/null || true)
[ "$CODE" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "dashboard login failed with HTTP $CODE"; }

ATTEMPT=1
MAX_ATTEMPTS=${STEP44_READINESS_ATTEMPTS:-6}
while :; do
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" "$BASE_URL/api/global-readiness" 2>/dev/null || true)
  if [ "$CODE" != "200" ]; then
    if [ "$ATTEMPT" -ge "$MAX_ATTEMPTS" ]; then
      cat "$BODY" >&2 2>/dev/null || true
      fail "global-readiness endpoint returned HTTP $CODE"
    fi
    info "global-readiness transport HTTP $CODE; retrying in 2s ($ATTEMPT/$MAX_ATTEMPTS)..."
    ATTEMPT=$((ATTEMPT + 1))
    sleep 2
    continue
  fi

  if python3 - "$BODY" <<'PY'
import json, sys
try:
    d=json.load(open(sys.argv[1], encoding='utf-8'))
except Exception:
    raise SystemExit(2)
if d.get('status') == 'VALIDATED_FAIL_CLOSED' and d.get('safeToContinueDashboard') is True:
    raise SystemExit(0)
blocking=int(d.get('blockingCount') or 0)
retryable=int(d.get('retryableBlockingCount') or 0)
if blocking > 0 and blocking == retryable:
    raise SystemExit(10)
raise SystemExit(20)
PY
  then
    break
  else
    RC=$?
    if [ "$RC" = "10" ]; then
      if [ "$ATTEMPT" -lt "$MAX_ATTEMPTS" ]; then
        info "only bounded transient read blocker(s) remain; retrying in 2s ($ATTEMPT/$MAX_ATTEMPTS)..."
        ATTEMPT=$((ATTEMPT + 1))
        sleep 2
        continue
      fi
      cat "$BODY" >&2
      fail "only retryable source blockers remain after $MAX_ATTEMPTS readiness snapshots; source contention/unavailability persisted"
    fi
    cat "$BODY" >&2
    fail "global readiness contains a non-transient blocker or incomplete contract"
  fi
done

python3 - "$BODY" <<'PY' || exit 1
import json, sys

d=json.load(open(sys.argv[1], encoding='utf-8'))
expected={
 'provider-real','postgres','nats','canonical-market-data','reconciliation',
 'durable-e2e-proof','venue-foundation','venue-public','venue-rules','symbol-registry',
 'ledger','watchdog','critical-alerts','manual-route','production-auth','private-auth',
 'account-snapshot','order-lifecycle','service-liveness','clock-sync','mock-exchange'
}
rows={r.get('id'):r for r in d.get('requirements',[])}
missing=sorted(expected-set(rows))
if missing:
    raise SystemExit('STEP44: FAIL: missing readiness requirement IDs: '+', '.join(missing))
if d.get('contractVersion') != 'step44-v1' or d.get('contractComplete') is not True:
    raise SystemExit('STEP44: FAIL: readiness contract is not complete/versioned step44-v1')
if d.get('status') != 'VALIDATED_FAIL_CLOSED' or d.get('safeToContinueDashboard') is not True:
    raise SystemExit('STEP44: FAIL: current dashboard phase is not safely validated')
for key in ('privateTestnetReady','tradingReady','liveReady'):
    if d.get(key) is not False:
        raise SystemExit(f'STEP44: FAIL: {key} must remain false')
if d.get('orderRouting') != 'DISABLED':
    raise SystemExit('STEP44: FAIL: order routing must remain DISABLED')
if d.get('manualRouting') != 'DISABLED':
    raise SystemExit('STEP44: FAIL: manual routing must remain DISABLED')
if d.get('privateAuth') not in ('DEFERRED','DISABLED'):
    raise SystemExit('STEP44: FAIL: private auth must remain deferred/disabled')
for req_id in ('provider-real','venue-foundation','venue-public','venue-rules','ledger','critical-alerts','manual-route'):
    if rows[req_id].get('state') != 'PASS':
        raise SystemExit(f'STEP44: FAIL: required current evidence {req_id} is {rows[req_id].get("state")}')
if rows['symbol-registry'].get('state') not in ('PASS','WARN'):
    raise SystemExit('STEP44: FAIL: symbol registry is not validated')
if rows['watchdog'].get('state') not in ('PASS','WARN'):
    raise SystemExit('STEP44: FAIL: watchdog is not current')
for req_id in ('private-auth','account-snapshot','order-lifecycle','service-liveness','clock-sync','mock-exchange'):
    if rows[req_id].get('state') != 'DEFERRED':
        raise SystemExit(f'STEP44: FAIL: {req_id} must be explicit DEFERRED, got {rows[req_id].get("state")}')
if 'MockExchangeAdapter' not in d.get('futureReplayBoundary',''):
    raise SystemExit('STEP44: FAIL: future mock-exchange replay boundary is missing')
print(f"STEP44: PASS: contract={d.get('contractVersion')} requirements={len(rows)} warnings={d.get('warningCount')} deferred={d.get('deferredCount')} blockers={d.get('blockingCount')}")
PY

echo "[4/8] Readiness semantics distinguish development from trading authorization"
grep -q 'VALIDATED_FAIL_CLOSED' "$ROOT/dashboard-api/internal/server/global_readiness.go" || fail "fail-closed contract status missing"
grep -q 'PrivateTestnetReady' "$ROOT/dashboard-api/internal/server/global_readiness.go" || fail "private-testnet readiness dimension missing"
grep -q 'TradingReady' "$ROOT/dashboard-api/internal/server/global_readiness.go" || fail "trading readiness dimension missing"
grep -q 'LiveReady' "$ROOT/dashboard-api/internal/server/global_readiness.go" || fail "LIVE readiness dimension missing"
grep -q 'mock-exchange' "$ROOT/dashboard-api/internal/server/global_readiness.go" || fail "future mock exchange boundary missing from readiness contract"
pass "dashboard continuation, private-testnet, trading, LIVE and future replay are explicit independent readiness dimensions"

echo "[5/8] Global readiness endpoint is authenticated GET-only"
UNAUTH_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' "$BASE_URL/api/global-readiness" 2>/dev/null || true)
[ "$UNAUTH_CODE" = "401" ] || fail "unauthenticated GET /api/global-readiness returned HTTP $UNAUTH_CODE; expected 401"
POST_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" -X POST "$BASE_URL/api/global-readiness" 2>/dev/null || true)
[ "$POST_CODE" = "405" ] || fail "POST /api/global-readiness returned HTTP $POST_CODE; expected 405"
pass "global readiness is authenticated and read-only"

echo "[6/8] Infrastructure UI exposes the Step 44 contract"
grep -q "getGlobalReadiness" "$ROOT/src/data/dashboardDataSource.ts" || fail "frontend data-source contract missing getGlobalReadiness"
grep -q "Step 44 · Full Global Readiness Contract" "$ROOT/src/pages/InfrastructurePage.tsx" || fail "Infrastructure Step 44 panel missing"
grep -q "futureReplayBoundary" "$ROOT/src/pages/InfrastructurePage.tsx" || fail "future mock/replay boundary is not visible in Infrastructure"
pass "Infrastructure exposes phase-scoped readiness and future replay boundary"

echo "[7/8] No private wallet/signing/order command surface introduced"
SERVER_GO="$ROOT/dashboard-api/internal/server/server.go"
[ -f "$SERVER_GO" ] || fail "server route registry is missing"

# Audit the actual HTTP route registry, not descriptive strings such as
# live_trading/exchange_gateway or documentation that merely mentions a future
# venue command endpoint. Through Step 46 the only mutating dashboard routes
# are authentication, manual-control PREVIEW, and fail-closed ROUTE ADMISSION.
MUTATING_ROUTES=$(grep -nE 'mux\.Handle(Func)?\("(POST|PUT|PATCH|DELETE) /api/' "$SERVER_GO" \
  | grep -vE 'POST /api/auth/(login|logout)|POST /api/manual-control/(preview|route)' || true)
if [ -n "$MUTATING_ROUTES" ]; then
  printf '%s\n' "$MUTATING_ROUTES" >&2
  fail "unexpected mutating API route is registered"
fi

if grep -nE 'mux\.Handle(Func)?\("(GET|POST|PUT|PATCH|DELETE) /api/(submit|cancel|order|orders|trade|exchange)(/|\"| )' "$SERVER_GO" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "trading command API route is registered"
fi

# Secret-bearing runtime identifiers remain forbidden in compiled dashboard Go
# sources. Natural-language documentation is intentionally not treated as a
# command surface.
if grep -RInE 'PRIVATE_KEY|MNEMONIC|SEED_PHRASE|HYPERLIQUID_[A-Z0-9_]*SECRET|API_WALLET_[A-Z0-9_]*KEY' \
  "$ROOT/dashboard-api/cmd" "$ROOT/dashboard-api/internal" --include='*.go' >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "secret-bearing runtime identifier detected in dashboard Go source"
fi

# This sentinel overwrites a historical Step 37 prototype when users extract a
# cumulative ZIP over an existing dashboard tree. The private integration is
# still deferred and no handler is registered for it.
TOMBSTONE="$ROOT/dashboard-api/internal/provider/venue_private_auth.go"
if [ -f "$TOMBSTONE" ] && ! grep -q 'step44DeferredPrivateAuthTombstone' "$TOMBSTONE"; then
  fail "stale Step 37 private-auth provider detected; replace dashboard/ with the cumulative Step 44.6 package"
fi

pass "Step 44 route registry remains read/preview/admission-only; no capital-moving path or secret loader exists"

echo "[8/8] Manual routing remains fail-closed"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP44: FAIL: manual-control routeEnabled=true')
print('STEP44: PASS: manual routing remains fail-closed')
PY

echo
echo "============================================================"
echo "STEP 44: PASS — FULL GLOBAL READINESS CONTRACT VALIDATED"
echo "============================================================"
echo "The dashboard now exposes one explicit phase-scoped readiness contract."
echo "Safe dashboard continuation does NOT imply private TESTNET, trading or LIVE readiness."
echo "Steps 37-41 remain DEFERRED, routing remains disabled, and no wallet/funds are required."
echo "Future full-replay support is pinned to a shared HyperliquidAdapter/MockExchangeAdapter contract."
echo "Current safe continuation is determined by the phase-scoped readiness contract."
