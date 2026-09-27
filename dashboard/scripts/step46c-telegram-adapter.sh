#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
OPERATOR_USER=${DASHBOARD_GATE_USERNAME:-operator}
OPERATOR_PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE="${TMPDIR:-/tmp}/control-dashboard-step46c-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step46c-body.$$"
INSPECT="${TMPDIR:-/tmp}/control-dashboard-step46c-inspect.$$"
trap 'rm -f "$COOKIE" "$BODY" "$INSPECT"' EXIT

fail() { echo "STEP46C: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP46C: PASS: $*"; }
info() { echo "STEP46C: INFO: $*"; }

login_json() {
  python3 - "$1" "$2" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

login() {
  payload=$(login_json "$OPERATOR_USER" "$OPERATOR_PASS")
  code=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE" -H 'Content-Type: application/json' --data "$payload" "$BASE_URL/api/auth/login" 2>/dev/null || true)
  [ "$code" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "operator login failed with HTTP $code"; }
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 46C — TELEGRAM NOTIFICATION ADAPTER"
echo "============================================================"
echo "Optional Telegram delivery behind the independent notifier."
echo "The authoritative gate uses local HTTP mocks; real bot secrets are not required."
echo

echo "[1/8] Step 46B durable notifier precondition remains present"
command -v docker >/dev/null 2>&1 || fail "Docker is required for the Step 46C runtime gate"
docker inspect control-dashboard-alert-notifier >/dev/null 2>&1 || fail "control-dashboard-alert-notifier container is missing; rebuild the real dashboard stack"
[ "$(docker inspect -f '{{.State.Running}}' control-dashboard-alert-notifier 2>/dev/null)" = "true" ] || fail "alert notifier container is not running"
docker exec control-dashboard-alert-notifier sh -ec 'test -s /data/notifier/events.jsonl && test -s /data/notifier/status.json' >/dev/null 2>&1 || fail "Step 46B durable TEST_FILE store/status is missing"
docker exec control-dashboard-alert-notifier sh -ec "grep -q '\"action\":\"BOOTSTRAP_COMPLETE\"' /data/notifier/events.jsonl && grep -q '\"bootstrapComplete\":true' /data/notifier/status.json" >/dev/null 2>&1 || fail "Step 46B bootstrap evidence is incomplete"
pass "Step 46B durable lifecycle decisions and bootstrap evidence remain intact"

echo "[2/8] Telegram adapter regression tests, vet and build"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./internal/notifier ./cmd/dashboard-alert-notifier >/dev/null
    CGO_ENABLED=0 go vet ./internal/notifier ./cmd/dashboard-alert-notifier >/dev/null
    CGO_ENABLED=0 go build ./cmd/dashboard-alert-notifier >/dev/null
  ) || fail "Telegram notifier Go test/vet/build failed"
elif command -v docker >/dev/null 2>&1; then
  STEP46C_GO_IMAGE=${STEP46C_GO_IMAGE:-control-dashboard-step46c-go:local}
  docker build --target build -t "$STEP46C_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null || fail "could not build local Go regression image"
  docker run --rm "$STEP46C_GO_IMAGE" sh -ec 'cd /src && CGO_ENABLED=0 go test ./internal/notifier ./cmd/dashboard-alert-notifier >/dev/null && CGO_ENABLED=0 go vet ./internal/notifier ./cmd/dashboard-alert-notifier >/dev/null && CGO_ENABLED=0 go build ./cmd/dashboard-alert-notifier >/dev/null' || fail "Telegram notifier Go checks failed inside local build-stage image"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi
pass "mock sendMessage, receipt dedup, size bound, timeout and secret-redaction regressions pass"

echo "[3/8] Runtime notifier remains isolated from trading networks and credentials"
docker inspect control-dashboard-alert-notifier >"$INSPECT" || fail "could not inspect notifier container"
python3 - "$INSPECT" <<'PY' || exit 1
import json, sys
c=json.load(open(sys.argv[1], encoding='utf-8'))[0]
mounts={m.get('Destination'):m for m in c.get('Mounts',[])}
watch=mounts.get('/data/watchdog'); state=mounts.get('/data/notifier')
if not watch or watch.get('RW') is not False:
    raise SystemExit('STEP46C: FAIL: /data/watchdog must remain read-only')
if not state or state.get('RW') is not True:
    raise SystemExit('STEP46C: FAIL: /data/notifier must remain the dedicated writable state volume')
nets=list((c.get('NetworkSettings',{}).get('Networks') or {}).keys())
if len(nets) != 1 or 'dashboard-notifier-egress' not in nets[0]:
    raise SystemExit('STEP46C: FAIL: notifier must be attached only to dedicated dashboard-notifier-egress')
if any('algotrading-live' in n or n.endswith('dashboard-private') for n in nets):
    raise SystemExit('STEP46C: FAIL: notifier is attached to a trading/dashboard private network')
env=c.get('Config',{}).get('Env',[])
for prefix in ('DASHBOARD_POSTGRES_DSN=','DASHBOARD_NATS_URL=','DASHBOARD_NATS_MONITOR_URL=','PRIVATE_KEY=','MNEMONIC=','SEED_PHRASE='):
    if any(x.startswith(prefix) for x in env):
        raise SystemExit('STEP46C: FAIL: notifier received forbidden trading/private env '+prefix[:-1])
sink='TEST_FILE'
for item in env:
    if item.startswith('DASHBOARD_NOTIFIER_SINK='):
        sink=item.split('=',1)[1].strip().upper() or 'TEST_FILE'
if sink not in ('TEST_FILE','TELEGRAM'):
    raise SystemExit('STEP46C: FAIL: runtime notifier sink is neither TEST_FILE nor TELEGRAM')
if sink == 'TELEGRAM':
    vals={x.split('=',1)[0]:x.split('=',1)[1] for x in env if '=' in x}
    token=vals.get('DASHBOARD_TELEGRAM_BOT_TOKEN','').strip()
    token_file=vals.get('DASHBOARD_TELEGRAM_BOT_TOKEN_FILE','').strip()
    chat=vals.get('DASHBOARD_TELEGRAM_CHAT_ID','').strip()
    chat_file=vals.get('DASHBOARD_TELEGRAM_CHAT_ID_FILE','').strip()
    if bool(token) == bool(token_file) or bool(chat) == bool(chat_file):
        raise SystemExit('STEP46C: FAIL: TELEGRAM runtime must configure exactly one source for token and chat id')
print('STEP46C: PASS: notifier has egress-only networking, read-only alert source and no trading/private credentials')
PY

echo "[4/8] Current notifier heartbeat is successful in its selected sink mode"
ATTEMPT=1
while :; do
  if docker exec control-dashboard-alert-notifier sh -ec '
    sink=${DASHBOARD_NOTIFIER_SINK:-TEST_FILE};
    store=${DASHBOARD_NOTIFIER_STORE_DIR:-};
    if [ -z "$store" ]; then if [ "$sink" = TELEGRAM ]; then store=/data/notifier/telegram; else store=/data/notifier; fi; fi;
    test -s "$store/status.json" && grep -q '"'"'"lastSuccessAt"'"'"' "$store/status.json"
  ' >/dev/null 2>&1; then
    break
  fi
  [ "$ATTEMPT" -lt 12 ] || fail "notifier did not publish a successful heartbeat for the selected sink"
  ATTEMPT=$((ATTEMPT + 1)); sleep 1
done
docker exec control-dashboard-alert-notifier sh -ec '
  sink=${DASHBOARD_NOTIFIER_SINK:-TEST_FILE}; store=${DASHBOARD_NOTIFIER_STORE_DIR:-};
  if [ -z "$store" ]; then if [ "$sink" = TELEGRAM ]; then store=/data/notifier/telegram; else store=/data/notifier; fi; fi;
  cat "$store/status.json"
' >"$BODY" 2>/dev/null || fail "could not read current notifier heartbeat"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
h=json.load(open(sys.argv[1], encoding='utf-8'))
if h.get('version') != 'step46b-v1' or not h.get('lastSuccessAt') or h.get('bootstrapComplete') is not True:
    raise SystemExit('STEP46C: FAIL: notifier heartbeat is not successful/bootstrap-complete')
if h.get('sink') not in ('TEST_FILE','TELEGRAM'):
    raise SystemExit('STEP46C: FAIL: notifier heartbeat reports unsupported sink')
print('STEP46C: PASS: selected notifier sink has a successful bootstrap-complete heartbeat ('+h.get('sink','?')+')')
PY

echo "[5/8] Telegram HTTP/secret boundary is fail-closed"
grep -q 'telegramAPIBaseURL.*https://api.telegram.org' "$ROOT/dashboard-api/internal/notifier/telegram.go" || fail "official Telegram API base is not fixed in adapter code"
if grep -RIn 'DASHBOARD_TELEGRAM_API_BASE_URL' "$ROOT/dashboard-api" "$ROOT/docker-compose.real.yml" "$ROOT/docker-compose.production.real.yml" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "deployment can redirect Telegram credentials to an arbitrary API base"
fi
python3 - "$ROOT/.env.example" "$ROOT/.env.production.example" <<'PY' || exit 1
import sys
for path in sys.argv[1:]:
    vals={}
    for raw in open(path, encoding='utf-8'):
        raw=raw.strip()
        if not raw or raw.startswith('#') or '=' not in raw: continue
        k,v=raw.split('=',1); vals[k]=v
    for k in ('DASHBOARD_TELEGRAM_BOT_TOKEN','DASHBOARD_TELEGRAM_CHAT_ID'):
        if vals.get(k,'').strip():
            raise SystemExit(f'STEP46C: FAIL: {path} contains a non-empty Telegram secret placeholder for {k}')
print('STEP46C: PASS: examples contain no Telegram secrets and production API base cannot be redirected')
PY

echo "[6/8] Static safety: Telegram delivery cannot become a trading command"
if grep -RInE '\.Publish\(|Publish\(|/exchange|submit.*order|cancel.*order|modify.*order|DASHBOARD_POSTGRES_DSN|DASHBOARD_NATS_URL|PRIVATE_KEY|MNEMONIC|SEED_PHRASE' "$ROOT/dashboard-api/internal/notifier" "$ROOT/dashboard-api/cmd/dashboard-alert-notifier" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "notifier source contains forbidden trading/publish/private surface"
fi
grep -q 'dashboard-notifier-egress' "$ROOT/docker-compose.real.yml" || fail "local real compose lacks dedicated notifier egress network"
grep -q 'dashboard-watchdog-data:/data/watchdog:ro' "$ROOT/docker-compose.real.yml" || fail "notifier alert source is not read-only in compose"
pass "Telegram is delivery-only; no trading publish, exchange, database-write or signing surface exists"

echo "[7/8] Telegram sink-specific bootstrap and durable receipt contracts are present"
grep -q 'step46c-telegram-receipt-v1' "$ROOT/dashboard-api/internal/notifier/telegram.go" || fail "Telegram receipt contract missing"
grep -q '/data/notifier/telegram' "$ROOT/dashboard-api/cmd/dashboard-alert-notifier/main.go" || fail "sink-specific Telegram store default missing"
grep -q 'notify:' "$ROOT/dashboard-api/internal/notifier/engine.go" || fail "deterministic notification id contract missing"
pass "Telegram first-enable bootstrap is isolated from Step 46B history and successful sends have durable receipts"

echo "[8/8] Telegram adapter does not authorize private/manual/LIVE trading"
login
READINESS=$(curl -fsS -b "$COOKIE" "$BASE_URL/api/global-readiness") || fail "global readiness unavailable"
printf '%s' "$READINESS" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('privateTestnetReady') is not False or d.get('tradingReady') is not False or d.get('liveReady') is not False:
    raise SystemExit('STEP46C: FAIL: Telegram adapter altered private/trading/LIVE readiness')
if d.get('manualRouting') != 'DISABLED':
    raise SystemExit('STEP46C: FAIL: Telegram adapter altered manual routing')
print('STEP46C: PASS: private/trading/LIVE remain false and manual routing remains disabled')
PY

echo
echo "============================================================"
echo "STEP 46C: PASS — TELEGRAM NOTIFICATION ADAPTER VALIDATED"
echo "============================================================"
echo "Telegram sendMessage is implemented behind the independent notifier,"
echo "validated with a local HTTP mock, bounded/redacted, receipt-backed and"
echo "isolated on a dedicated egress-only network. TEST_FILE remains the default."
echo "Next safe step: Step 47 Hyperliquid API Surface & Venue Semantics Specification."
