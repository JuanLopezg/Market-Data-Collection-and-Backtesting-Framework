#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step34-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step34-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP34: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP34: PASS: $*"; }
info() { echo "STEP34: INFO: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 34 — PUBLIC VENUE CONNECTIVITY / METADATA"
echo "============================================================"
echo "Public Hyperliquid TESTNET info only: NO secrets, signing, private account calls or orders."
echo

echo "[1/6] Step 33 foundation precondition"
"$ROOT/scripts/step33-testnet-foundation.sh" >/dev/null || fail "Step 33 foundation gate failed"
pass "Step 33 foundation remains valid"

echo "[2/6] Public client safety boundary"
CLIENT="$ROOT/dashboard-api/internal/integration/hyperliquid/public_client.go"
[ -f "$CLIENT" ] || fail "Hyperliquid public client source is missing"
grep -q '"type": "meta"' "$CLIENT" || fail "fixed meta public query missing"
grep -q '"type": "allMids"' "$CLIENT" || fail "fixed allMids public query missing"
if grep -Eq 'Authorization|privateKey|secret|/exchange' "$CLIENT"; then
  fail "public client contains an auth/secret/order surface"
fi
pass "public adapter is limited to fixed info queries and sends no auth/order request"

echo "[3/6] Exact TESTNET endpoint configuration"
grep -q 'DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL: "https://api.hyperliquid-testnet.xyz/info"' "$ROOT/docker-compose.real.yml" \
  || fail "local real compose does not pin the Hyperliquid TESTNET info endpoint"
pass "public venue endpoint is pinned to Hyperliquid TESTNET /info"

echo "[4/6] Authenticated public venue probe"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login") \
  || fail "cannot reach dashboard login at $BASE_URL"
[ "$CODE" = "200" ] || { cat "$BODY" >&2; fail "dashboard login returned HTTP $CODE"; }

ATTEMPTS=${STEP34_PUBLIC_ATTEMPTS:-5}
RETRY_SECONDS=${STEP34_PUBLIC_RETRY_SECONDS:-2}
attempt=1
while :; do
  PUBLIC=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/venue-public") || fail "venue-public endpoint unavailable"
  printf '%s' "$PUBLIC" >"$BODY"
  if python3 - "$BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('connected') is not True or d.get('status') != 'CONNECTED_PUBLIC_ONLY':
    print(json.dumps(d, indent=2), file=sys.stderr)
    raise SystemExit(10)
expected={
    'venue':'HYPERLIQUID',
    'targetEnvironment':'TESTNET',
    'metadataStatus':'AVAILABLE',
    'midsStatus':'AVAILABLE',
    'privateAuth':'DISABLED',
    'orderRouting':'DISABLED',
    'symbolMapping':'NOT_CONFIGURED',
    'exchangeFilters':'NOT_APPLIED',
}
for key,value in expected.items():
    if d.get(key) != value:
        raise SystemExit(f'STEP34: FAIL: {key}={d.get(key)!r}, want {value!r}')
if d.get('secretsUsed') or d.get('capitalUsed') or not d.get('readOnly'):
    raise SystemExit('STEP34: FAIL: public probe crossed the no-secret/no-capital/read-only boundary')
if int(d.get('universeCount') or 0) <= 0 or int(d.get('midCount') or 0) <= 0 or int(d.get('matchedMidCount') or 0) <= 0:
    raise SystemExit('STEP34: FAIL: public metadata/mids are empty or inconsistent')
print(f"STEP34: PASS: public TESTNET connected; universe={d['universeCount']} mids={d['midCount']} overlap={d['matchedMidCount']} latencyMs={d.get('latencyMs')}")
PY
  then
    break
  else
    rc=$?
  fi
  [ "$rc" -eq 10 ] || fail "public venue payload violated the Step 34 contract"
  if [ "$attempt" -ge "$ATTEMPTS" ]; then
    fail "Hyperliquid TESTNET public connectivity/metadata did not become available after $ATTEMPTS attempts"
  fi
  info "public probe not ready; retrying in ${RETRY_SECONDS}s (attempt $attempt/$ATTEMPTS)"
  sleep "$RETRY_SECONDS"
  attempt=$((attempt + 1))
done

echo "[5/6] Execution connectivity remains fail-closed"
INFRA=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/infrastructure") || fail "infrastructure endpoint unavailable"
printf '%s' "$INFRA" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
e=d.get('exchange') or {}
if e.get('connected') is True or e.get('state') != 'UNKNOWN':
    raise SystemExit(f"STEP34: FAIL: public metadata probe must not promote execution connectivity: {e}")
print('STEP34: PASS: public connectivity is not misrepresented as execution/private connectivity')
PY

echo "[6/6] Manual route remains disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP34: FAIL: manual-control routeEnabled=true')
print('STEP34: PASS: manual routing remains fail-closed')
PY

echo
echo "============================================================"
echo "STEP 34: PASS — PUBLIC HYPERLIQUID TESTNET CONNECTIVITY VERIFIED"
echo "============================================================"
echo "Public metadata is reachable and structurally usable."
echo "Next: Step 35 may add EXPLICIT symbol mapping validation only."
echo "Private auth, signing, submit/cancel and real capital remain out of scope."
