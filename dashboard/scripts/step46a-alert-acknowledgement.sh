#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
OPERATOR_USER=${DASHBOARD_GATE_USERNAME:-operator}
OPERATOR_PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
VIEWER_USER=${DASHBOARD_GATE_VIEWER_USERNAME:-viewer}
VIEWER_PASS=${DASHBOARD_GATE_VIEWER_PASSWORD:-viewer-demo}
OP_COOKIE="${TMPDIR:-/tmp}/control-dashboard-step46a-op-cookies.$$"
VIEW_COOKIE="${TMPDIR:-/tmp}/control-dashboard-step46a-view-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step46a-body.$$"
ALERTS="${TMPDIR:-/tmp}/control-dashboard-step46a-alerts.$$"
PAYLOAD="${TMPDIR:-/tmp}/control-dashboard-step46a-payload.$$"
SELECTED="${TMPDIR:-/tmp}/control-dashboard-step46a-selected.$$"
trap 'rm -f "$OP_COOKIE" "$VIEW_COOKIE" "$BODY" "$ALERTS" "$PAYLOAD" "$SELECTED"' EXIT

fail() { echo "STEP46A: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP46A: PASS: $*"; }
info() { echo "STEP46A: INFO: $*"; }

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
echo "CONTROL DASHBOARD STEP 46A — DURABLE HUMAN ALERT ACKNOWLEDGEMENT"
echo "============================================================"
echo "Append-only OPERATOR acknowledgement bound to the exact durable alert"
echo "lifecycle instance. ACK never resolves an alert, clears readiness, publishes"
echo "a trading command, signs anything or moves capital."
echo

echo "[1/9] Step 46 dashboard-complete precondition"
"$ROOT/scripts/step46-manual-control-safe-routing.sh" >/dev/null || fail "Step 46 Manual Control gate failed"
pass "Step 46 remains valid; routing stays fail-closed"

echo "[2/9] Go regression + acknowledgement lifecycle tests"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
elif command -v docker >/dev/null 2>&1; then
  STEP46A_GO_IMAGE=${STEP46A_GO_IMAGE:-control-dashboard-step46a-go:local}
  docker build --target build -t "$STEP46A_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null || fail "could not build local Go regression image"
  docker run --rm "$STEP46A_GO_IMAGE" sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' || fail "Go test/vet/build failed inside local build-stage image"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi
pass "append-only store, lifecycle binding, stale-key and idempotency regressions pass"

echo "[3/9] REAL Alerts & Audit exposes Step 46A acknowledgement contract"
OP_CSRF=$(login "$OPERATOR_USER" "$OPERATOR_PASS" "$OP_COOKIE") || fail "operator login/CSRF failed"
ATTEMPT=1
while :; do
  CODE=$(curl -sS -o "$ALERTS" -w '%{http_code}' -b "$OP_COOKIE" "$BASE_URL/api/alerts-audit" 2>/dev/null || true)
  if [ "$CODE" = "200" ] && python3 - "$ALERTS" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
assert d.get('sourceMode') == 'REAL'
assert d.get('acknowledgementAvailable') is True
assert isinstance(d.get('acknowledged'), int) and d.get('acknowledged') >= 0
assert isinstance(d.get('acknowledgementEventCount'), int) and d.get('acknowledgementEventCount') >= 0
assert 'DURABLE_ALERT_ACK' in str(d.get('auditMode') or '')
note=str(d.get('sourceNote') or '')
assert 'Step 46A' in note and 'ACK never resolves' in note
PY
  then break; fi
  [ "$ATTEMPT" -lt 6 ] || { cat "$ALERTS" >&2 2>/dev/null || true; fail "Step 46A acknowledgement projection did not become available"; }
  info "acknowledgement projection not ready yet; retrying in 2s ($ATTEMPT/6)..."
  ATTEMPT=$((ATTEMPT + 1)); sleep 2
done
pass "acknowledgement store is available and explicitly non-resolving"

echo "[4/9] Authentication, role, CSRF and stale-lifecycle boundary"
UNAUTH_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -H 'Content-Type: application/json' --data '{"alertId":"missing","acknowledgementKey":"stale"}' "$BASE_URL/api/alerts-audit/acknowledge" 2>/dev/null || true)
[ "$UNAUTH_CODE" = "401" ] || fail "unauthenticated acknowledgement returned HTTP $UNAUTH_CODE; expected 401"
VIEW_CSRF=$(login "$VIEWER_USER" "$VIEWER_PASS" "$VIEW_COOKIE") || fail "viewer login/CSRF failed"
VIEW_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$VIEW_COOKIE" -H "X-CSRF-Token: $VIEW_CSRF" -H 'Content-Type: application/json' --data '{"alertId":"missing","acknowledgementKey":"stale"}' "$BASE_URL/api/alerts-audit/acknowledge" 2>/dev/null || true)
[ "$VIEW_CODE" = "403" ] || fail "VIEWER acknowledgement returned HTTP $VIEW_CODE; expected 403"
NO_CSRF_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H 'Content-Type: application/json' --data '{"alertId":"missing","acknowledgementKey":"stale"}' "$BASE_URL/api/alerts-audit/acknowledge" 2>/dev/null || true)
[ "$NO_CSRF_CODE" = "403" ] || fail "OPERATOR acknowledgement without CSRF returned HTTP $NO_CSRF_CODE; expected 403"
MISSING_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H "X-CSRF-Token: $OP_CSRF" -H 'Content-Type: application/json' --data '{"alertId":"missing","acknowledgementKey":"stale"}' "$BASE_URL/api/alerts-audit/acknowledge" 2>/dev/null || true)
[ "$MISSING_CODE" = "404" ] || fail "unknown alert acknowledgement returned HTTP $MISSING_CODE; expected 404"
pass "only authenticated OPERATOR + CSRF can target a current durable alert instance"

echo "[5/9] Runtime acknowledgement is append-only and lifecycle-bound"
python3 - "$ALERTS" >"$SELECTED" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
for a in d.get('alerts') or []:
    if a.get('status') in ('ACTIVE','ACKNOWLEDGED') and str(a.get('acknowledgementKey') or '').strip():
        print(json.dumps({
            'alertId':a['id'],
            'acknowledgementKey':a['acknowledgementKey'],
            'status':a.get('status'),
            'severity':a.get('severity'),
        }, ensure_ascii=False))
        break
PY

if [ -s "$SELECTED" ]; then
  python3 - "$SELECTED" >"$PAYLOAD" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
print(json.dumps({'alertId':d['alertId'],'acknowledgementKey':d['acknowledgementKey'],'comment':'Step 46A gate acknowledgement'}, ensure_ascii=False))
PY
  BEFORE_LINES=$(docker exec control-dashboard-api sh -ec 'if [ -f /data/alert-ack/events.jsonl ]; then wc -l < /data/alert-ack/events.jsonl; else echo 0; fi' 2>/dev/null || echo unknown)
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H "X-CSRF-Token: $OP_CSRF" -H 'Content-Type: application/json' --data-binary @"$PAYLOAD" "$BASE_URL/api/alerts-audit/acknowledge" 2>/dev/null || true)
  [ "$CODE" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "runtime acknowledgement returned HTTP $CODE"; }
  python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('status') != 'ACKNOWLEDGED' or d.get('persisted') is not True:
    raise SystemExit('STEP46A: FAIL: acknowledgement response is not durably persisted')
if d.get('resolutionState') != 'UNRESOLVED' or d.get('tradingStateMutated') is not False:
    raise SystemExit('STEP46A: FAIL: acknowledgement response implies resolution/trading mutation')
if not str(d.get('correlationId') or '').startswith('ack-'):
    raise SystemExit('STEP46A: FAIL: acknowledgement correlation id is missing')
print('STEP46A: PASS: runtime acknowledgement persisted without resolution/trading mutation')
PY
  FIRST_RESPONSE=$(cat "$BODY")
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$OP_COOKIE" -H "X-CSRF-Token: $OP_CSRF" -H 'Content-Type: application/json' --data-binary @"$PAYLOAD" "$BASE_URL/api/alerts-audit/acknowledge" 2>/dev/null || true)
  [ "$CODE" = "200" ] || fail "idempotent acknowledgement returned HTTP $CODE"
  python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('alreadyAcknowledged') is not True:
    raise SystemExit('STEP46A: FAIL: repeated acknowledgement was not idempotent')
PY
  AFTER_LINES=$(docker exec control-dashboard-api sh -ec 'if [ -f /data/alert-ack/events.jsonl ]; then wc -l < /data/alert-ack/events.jsonl; else echo 0; fi' 2>/dev/null || echo unknown)
  if [ "$BEFORE_LINES" != "unknown" ] && [ "$AFTER_LINES" != "unknown" ]; then
    ALREADY=$(python3 -c 'import json,sys; print(str(json.loads(sys.stdin.read()).get("alreadyAcknowledged",False)).lower())' <<EOF2
$FIRST_RESPONSE
EOF2
)
    if [ "$ALREADY" = "false" ]; then
      [ "$AFTER_LINES" -eq $((BEFORE_LINES + 1)) ] || fail "fresh acknowledgement did not append exactly one event ($BEFORE_LINES -> $AFTER_LINES)"
    else
      [ "$AFTER_LINES" -eq "$BEFORE_LINES" ] || fail "already-acknowledged lifecycle appended a duplicate event"
    fi
  fi
  pass "runtime ACK is durable and repeated ACK is idempotent"
else
  info "no current alert has a watchdog-stable acknowledgement key; runtime mutation is intentionally skipped"
  pass "empty/quiet runtime is valid; server/store lifecycle mutation is proven by Go regression"
fi

echo "[6/9] Persisted acknowledgement store is isolated and versioned"
command -v docker >/dev/null 2>&1 || fail "Docker is required to inspect the acknowledgement volume"
docker exec control-dashboard-api sh -ec 'test -d /data/alert-ack' >/dev/null 2>&1 || fail "dedicated /data/alert-ack volume is not mounted"
if docker exec control-dashboard-api sh -ec 'test -s /data/alert-ack/events.jsonl' >/dev/null 2>&1; then
  docker exec control-dashboard-api sh -ec 'tail -n 1 /data/alert-ack/events.jsonl' >"$BODY" || fail "could not read persisted acknowledgement event"
  python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('version') != 'step46a-v1' or d.get('action') != 'ALERT_ACKNOWLEDGE' or d.get('result') != 'SUCCESS':
    raise SystemExit('STEP46A: FAIL: persisted acknowledgement event contract/version mismatch')
if not str(d.get('lifecycleEventId') or '').strip() or not str(d.get('correlationId') or '').strip():
    raise SystemExit('STEP46A: FAIL: persisted acknowledgement lacks lifecycle/correlation binding')
print('STEP46A: PASS: persisted ACK store event is step46a-v1 and lifecycle-bound')
PY
else
  grep -q 'StoreVersion.*step46a-v1' "$ROOT/dashboard-api/internal/alertack/store.go" || fail "Step 46A store version constant missing"
  pass "ACK store is mounted and versioned; no event exists because runtime had no ackable alert"
fi

echo "[7/9] ACK remains unresolved and cannot authorize trading"
ATTEMPT=1
while :; do
  CODE=$(curl -sS -o "$ALERTS" -w '%{http_code}' -b "$OP_COOKIE" "$BASE_URL/api/alerts-audit" 2>/dev/null || true)
  if [ "$CODE" = "200" ]; then break; fi
  [ "$ATTEMPT" -lt 4 ] || fail "Alerts & Audit became unreadable after acknowledgement"
  ATTEMPT=$((ATTEMPT + 1)); sleep 1
done
python3 - "$ALERTS" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
for a in d.get('alerts') or []:
    if a.get('status') == 'ACKNOWLEDGED' and not str(a.get('acknowledgementKey') or '').strip():
        raise SystemExit('STEP46A: FAIL: acknowledged alert lacks durable lifecycle key')
PY
READINESS=$(curl -fsS -b "$OP_COOKIE" "$BASE_URL/api/global-readiness") || fail "global readiness unavailable"
printf '%s' "$READINESS" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('privateTestnetReady') is not False or d.get('tradingReady') is not False or d.get('liveReady') is not False:
    raise SystemExit('STEP46A: FAIL: acknowledgement altered private/trading/LIVE readiness')
if d.get('manualRouting') != 'DISABLED':
    raise SystemExit('STEP46A: FAIL: acknowledgement altered manual routing')
print('STEP46A: PASS: private/trading/LIVE remain false and routing remains disabled')
PY

echo "[8/9] Static safety + volume isolation boundary"
SERVER_GO="$ROOT/dashboard-api/internal/server/server.go"
grep -q 'POST /api/alerts-audit/acknowledge' "$SERVER_GO" || fail "acknowledgement route is missing"
if grep -RInE '\.Publish\(|Publish\(|/exchange|submit.*order|cancel.*order|PRIVATE_KEY|MNEMONIC|SEED_PHRASE' "$ROOT/dashboard-api/internal/alertack" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "acknowledgement store contains trading/publish/secret surface"
fi
grep -q 'dashboard-alert-ack-data:/data/alert-ack' "$ROOT/docker-compose.real.yml" || fail "dedicated alert ACK volume mount missing"
if grep -A35 'dashboard-watchdog:' "$ROOT/docker-compose.real.yml" | grep -q '/data/alert-ack'; then
  fail "watchdog must not write the human acknowledgement volume"
fi
pass "ACK persistence is isolated from watchdog/trading state and has no trading command surface"

echo "[9/9] Alerts UI exposes OPERATOR acknowledgement; Telegram/notifier stay deferred"
grep -q 'acknowledgeAlert' "$ROOT/src/data/dashboardDataSource.ts" || fail "frontend acknowledgement data-source contract missing"
grep -q 'Acknowledge' "$ROOT/src/pages/AlertsAuditPage.tsx" || fail "Alerts & Audit acknowledgement action missing"
grep -q 'OPERATOR required to acknowledge' "$ROOT/src/pages/AlertsAuditPage.tsx" || fail "VIEWER acknowledgement boundary is not visible in UI"
MANUAL=$(curl -fsS -b "$OP_COOKIE" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is not False:
    raise SystemExit('STEP46A: FAIL: manual routing became enabled')
PY
pass "human ACK is available; notifier/Telegram/private venue routing remain separate future steps"

echo
echo "============================================================"
echo "STEP 46A: PASS — DURABLE HUMAN ALERT ACKNOWLEDGEMENT VALIDATED"
echo "============================================================"
echo "OPERATOR acknowledgements are append-only and bound to the exact durable"
echo "watchdog lifecycle instance. ACK does not resolve alerts, suppress critical"
echo "readiness, publish trading commands or move capital."
echo "Next safe step: Step 46B Independent Alert Notifier."
