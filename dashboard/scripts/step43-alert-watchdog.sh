#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step43-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step43-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP43: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP43: PASS: $*"; }
info() { echo "STEP43: INFO: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 43 — DURABLE ALERTS / WATCHDOG"
echo "============================================================"
echo "Independent read-only watchdog + append-only alert lifecycle store."
echo "NO wallet, private auth, signing, order routing or capital movement."
echo

echo "[1/8] Step 42 ledger precondition"
"$ROOT/scripts/step42-ledger-foundation.sh" >/dev/null || fail "Step 42 append-only ledger gate failed"
pass "Step 42 remains valid; private Steps 37-41 stay deferred"

echo "[2/8] Go regression + durable lifecycle unit tests"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
elif command -v docker >/dev/null 2>&1; then
  STEP43_GO_IMAGE=${STEP43_GO_IMAGE:-control-dashboard-step43-go:local}
  docker build --target build -t "$STEP43_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null || fail "could not build local Go regression image"
  docker run --rm "$STEP43_GO_IMAGE" sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' || fail "Go test/vet/build failed inside local build-stage image"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi
pass "watchdog store open/update/resolve/restart tests and full Go regression pass"

echo "[3/8] Watchdog process/container safety boundary"
grep -q 'go build.*dashboard-watchdog' "$ROOT/dashboard-api/Dockerfile" || fail "dashboard-watchdog binary is not built"
grep -q '^  dashboard-watchdog:' "$ROOT/docker-compose.real.yml" || fail "dashboard-watchdog service missing from real compose"
grep -q 'entrypoint: \["/dashboard-watchdog"\]' "$ROOT/docker-compose.real.yml" || fail "watchdog entrypoint is not pinned"
grep -q 'dashboard-watchdog-data:/data/watchdog' "$ROOT/docker-compose.real.yml" || fail "dedicated durable watchdog volume missing"
if sed -n '/^  dashboard-watchdog:/,/^[^ ]/p' "$ROOT/docker-compose.real.yml" | grep -qE '^[[:space:]]+ports:'; then
  fail "dashboard-watchdog must not expose a public port"
fi
if grep -REn 'PRIVATE_KEY|mnemonic|seed phrase|/exchange|submit.*order|cancel.*order' "$ROOT/dashboard-api/cmd/dashboard-watchdog" "$ROOT/dashboard-api/internal/alertstore" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "watchdog/store contains a wallet/signing/order surface"
fi
pass "watchdog is a separate no-port process and writes only its dedicated observability volume"

echo "[4/8] Alert derivation includes registry + ledger integrity without trading mutation"
grep -q 'mergeSymbolRegistryAlerts' "$ROOT/dashboard-api/internal/provider/real_alerts_audit.go" || fail "symbol-registry alert integration missing"
grep -q 'mergeLedgerAlerts' "$ROOT/dashboard-api/internal/provider/real_alerts_audit.go" || fail "ledger integrity alert integration missing"
grep -q 'LEDGER_INTEGRITY_BLOCKED' "$ROOT/dashboard-api/internal/provider/real_alerts_audit.go" || fail "critical ledger integrity alarm missing"
grep -q 'SYMBOL_REGISTRY_BLOCKED' "$ROOT/dashboard-api/internal/provider/real_alerts_audit.go" || fail "symbol registry alarm missing"
pass "symbol registry and ledger integrity feed the common alert projection"

echo "[5/8] Authenticated watchdog heartbeat + durable history"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login" 2>/dev/null || true)
[ "$CODE" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "dashboard login failed with HTTP $CODE"; }

ATTEMPT=1
MAX_ATTEMPTS=${STEP43_WATCHDOG_ATTEMPTS:-15}
while :; do
  ALERTS=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/alerts-audit" 2>/dev/null || true)
  printf '%s' "$ALERTS" >"$BODY"
  if python3 - "$BODY" <<'PY'
import json, sys
try:
    d=json.load(open(sys.argv[1], encoding='utf-8'))
except Exception:
    raise SystemExit(1)
if d.get('watchdogAvailable') is not True or d.get('durableAlertHistoryAvailable') is not True:
    raise SystemExit(1)
if not d.get('watchdogLastSuccessAt'):
    raise SystemExit(1)
raise SystemExit(0)
PY
  then
    break
  fi
  if [ "$ATTEMPT" -ge "$MAX_ATTEMPTS" ]; then
    cat "$BODY" >&2 2>/dev/null || true
    if command -v docker >/dev/null 2>&1; then
      docker logs --tail 80 control-dashboard-watchdog >&2 2>/dev/null || true
    fi
    fail "durable alert watchdog did not publish a successful heartbeat"
  fi
  info "waiting 2s for durable watchdog heartbeat ($ATTEMPT/$MAX_ATTEMPTS)..."
  ATTEMPT=$((ATTEMPT + 1))
  sleep 2
done

python3 - "$BODY" <<'PY' || exit 1
import json, sys
from datetime import datetime, timezone

d=json.load(open(sys.argv[1], encoding='utf-8'))
state=d.get('watchdogState')
if state not in ('HEALTHY','DEGRADED'):
    print(json.dumps(d, indent=2, ensure_ascii=False), file=sys.stderr)
    raise SystemExit(f'STEP43: FAIL: watchdog state={state}')
ack_available=d.get('acknowledgementAvailable')
if ack_available not in (False, True):
    raise SystemExit('STEP43: FAIL: malformed acknowledgementAvailable flag')
if ack_available is True:
    mode=str(d.get('auditMode') or '')
    note=str(d.get('sourceNote') or '')
    if 'DURABLE_ALERT_ACK' not in mode:
        raise SystemExit('STEP43: FAIL: acknowledgement is available without the dedicated durable ACK audit mode')
    if 'Step 46A' not in note or 'ACK never resolves' not in note:
        raise SystemExit('STEP43: FAIL: acknowledgement availability lacks the Step 46A non-resolution disclosure')
    if not isinstance(d.get('acknowledged'), int) or d.get('acknowledged') < 0:
        raise SystemExit('STEP43: FAIL: invalid acknowledged count')

# Step 43 itself never fabricates human audit. A later Step 46 may legitimately
# attach the dedicated append-only manual operator-intent store to this same
# Alerts & Audit projection. Accept that forward-compatible capability only
# when it is explicitly identified as durable manual intent and every HUMAN
# record carries the Step 46 route-admission evidence fields.
human_available=d.get('humanAuditAvailable')
if human_available not in (False, True):
    raise SystemExit('STEP43: FAIL: malformed humanAuditAvailable flag')
if human_available is True:
    mode=str(d.get('auditMode') or '')
    note=str(d.get('sourceNote') or '')
    if 'DURABLE_MANUAL_INTENT' not in mode:
        raise SystemExit('STEP43: FAIL: human audit is available without the durable manual-intent audit mode')
    if 'operator-intent audit store' not in note:
        raise SystemExit('STEP43: FAIL: human audit is available without its dedicated append-only operator-intent source disclosure')
    for row in d.get('audit') or []:
        if row.get('actorType') != 'HUMAN':
            continue
        action=row.get('action')
        if action == 'MANUAL_ROUTE_ADMISSION':
            if not str(row.get('correlationId') or '').strip() or not str(row.get('requestHash') or '').strip():
                raise SystemExit('STEP43: FAIL: durable manual-route HUMAN audit row lacks correlation/hash evidence')
        elif action == 'ALERT_ACKNOWLEDGE' and ack_available is True:
            if not str(row.get('correlationId') or '').strip() or not str(row.get('target') or '').strip():
                raise SystemExit('STEP43: FAIL: durable alert-ack HUMAN audit row lacks correlation/target evidence')
        else:
            raise SystemExit('STEP43: FAIL: unexpected HUMAN audit action in the Step 43 forward-compatibility boundary')
last=d.get('watchdogLastSuccessAt')
try:
    ts=datetime.fromisoformat(last.replace('Z','+00:00'))
except Exception:
    raise SystemExit('STEP43: FAIL: malformed watchdogLastSuccessAt')
age=(datetime.now(timezone.utc)-ts).total_seconds()
if age > 120:
    raise SystemExit(f'STEP43: FAIL: watchdog heartbeat is stale ({age:.0f}s)')
if int(d.get('durableEventCount') or 0) < 0:
    raise SystemExit('STEP43: FAIL: invalid durable event count')
print(f"STEP43: PASS: watchdog={state} durableEvents={d.get('durableEventCount',0)} resolved24h={d.get('resolved24h',0)} lastSuccessAge={age:.1f}s")
PY

echo "[6/8] Persisted watchdog store is readable and versioned"
if ! command -v docker >/dev/null 2>&1; then
  fail "Docker is required to inspect the local watchdog persistence boundary"
fi
STATUS=$(docker exec control-dashboard-watchdog sh -ec 'cat /data/watchdog/status.json' 2>/dev/null) || fail "watchdog status.json is not readable from its dedicated volume"
printf '%s' "$STATUS" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('version') != 'step43-v1':
    raise SystemExit(f"STEP43: FAIL: unexpected watchdog store version {d.get('version')}")
if not d.get('lastSuccessAt'):
    raise SystemExit('STEP43: FAIL: watchdog durable heartbeat has no lastSuccessAt')
print(f"STEP43: PASS: durable store version={d.get('version')} events={d.get('eventCount')} active={d.get('activeCount')}")
PY

echo "[7/8] Alerts endpoint remains authenticated GET-only; trading DB adapters stay read-only"
POST_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" -X POST "$BASE_URL/api/alerts-audit" 2>/dev/null || true)
[ "$POST_CODE" = "405" ] || fail "POST /api/alerts-audit returned HTTP $POST_CODE; expected 405"
if grep -RInE '\b(INSERT|UPDATE|DELETE|ALTER|DROP|TRUNCATE|CREATE[[:space:]]+TABLE)\b' "$ROOT/dashboard-api/internal/integration/postgres" --include='*.go' --exclude='*_test.go' >"$BODY"; then
  cat "$BODY" >&2
  fail "mutating SQL found in trading PostgreSQL adapter"
fi
pass "dashboard alert API is GET-only; watchdog persistence is isolated from trading PostgreSQL"

echo "[8/8] Manual route and private venue path remain disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP43: FAIL: manual-control routeEnabled=true')
print('STEP43: PASS: manual routing remains fail-closed')
PY

if grep -RInE 'POST /api/(submit|cancel|order)|PRIVATE_KEY|mnemonic|seed phrase' "$ROOT/dashboard-api/internal/server" "$ROOT/dashboard-api/cmd/dashboard-watchdog" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "private/order command surface detected"
fi

echo
echo "============================================================"
echo "STEP 43: PASS — DURABLE ALERTS / WATCHDOG FOUNDATION VALIDATED"
echo "============================================================"
echo "A separate watchdog persists alert OPENED/UPDATED/RESOLVED transitions"
echo "to its own append-only observability store; the dashboard reads that history."
echo "Symbol-registry and ledger-integrity alarms are included. A later Step 46A"
echo "durable human-acknowledgement store is permitted, but ACK never resolves an"
echo "alert or changes trading readiness. Telegram delivery remains deferred."
echo "Step 37 private auth and Steps 38-41 remain DEFERRED; no wallet/funds required."
echo "Next safe step: Step 44 Full Global Readiness Contract."
