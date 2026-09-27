#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
OPERATOR_USER=${DASHBOARD_GATE_USERNAME:-operator}
OPERATOR_PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE="${TMPDIR:-/tmp}/control-dashboard-step46b-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step46b-body.$$"
STATUS="${TMPDIR:-/tmp}/control-dashboard-step46b-status.$$"
BEFORE_DECISIONS="${TMPDIR:-/tmp}/control-dashboard-step46b-decisions-before.$$"
AFTER_DECISIONS="${TMPDIR:-/tmp}/control-dashboard-step46b-decisions-after.$$"
BEFORE_SINK="${TMPDIR:-/tmp}/control-dashboard-step46b-sink-before.$$"
AFTER_SINK="${TMPDIR:-/tmp}/control-dashboard-step46b-sink-after.$$"
trap 'rm -f "$COOKIE" "$BODY" "$STATUS" "$BEFORE_DECISIONS" "$AFTER_DECISIONS" "$BEFORE_SINK" "$AFTER_SINK"' EXIT

fail() { echo "STEP46B: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP46B: PASS: $*"; }
info() { echo "STEP46B: INFO: $*"; }

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

read_notifier_file() {
  path=$1
  out=$2
  docker exec control-dashboard-alert-notifier sh -ec "if [ -f '$path' ]; then cat '$path'; fi" >"$out" 2>/dev/null || fail "could not read notifier file $path"
}

assert_unique_contracts() {
  decisions=$1
  sink=$2
  python3 - "$decisions" "$sink" <<'PY'
import json, sys
from collections import Counter

decisions=[]
with open(sys.argv[1], encoding='utf-8') as f:
    for raw in f:
        raw=raw.strip()
        if raw:
            decisions.append(json.loads(raw))
if not decisions:
    raise SystemExit('STEP46B: FAIL: notifier decision store is empty; bootstrap marker missing')
if any(d.get('version') != 'step46b-v1' for d in decisions):
    raise SystemExit('STEP46B: FAIL: notifier decision store version mismatch')
bootstrap=[d for d in decisions if d.get('action') == 'BOOTSTRAP_COMPLETE']
if len(bootstrap) != 1:
    raise SystemExit(f'STEP46B: FAIL: expected exactly one bootstrap marker, got {len(bootstrap)}')
source=[d.get('sourceEventId') for d in decisions if d.get('action') in ('DELIVERED','SUPPRESSED')]
if any(not x for x in source):
    raise SystemExit('STEP46B: FAIL: notifier decision lacks sourceEventId')
if len(source) != len(set(source)):
    raise SystemExit('STEP46B: FAIL: duplicate lifecycle source decision detected')

delivered={d.get('notificationId'): d for d in decisions if d.get('action') == 'DELIVERED'}
if any(not k for k in delivered):
    raise SystemExit('STEP46B: FAIL: delivered decision lacks notificationId')

notifications=[]
with open(sys.argv[2], encoding='utf-8') as f:
    for raw in f:
        raw=raw.strip()
        if raw:
            notifications.append(json.loads(raw))
ids=[n.get('notificationId') for n in notifications]
if any(not x for x in ids):
    raise SystemExit('STEP46B: FAIL: TEST_FILE notification lacks notificationId')
if len(ids) != len(set(ids)):
    raise SystemExit('STEP46B: FAIL: duplicate TEST_FILE notification detected')
for n in notifications:
    if n.get('version') != 'step46b-notification-v1':
        raise SystemExit('STEP46B: FAIL: TEST_FILE notification version mismatch')
    d=delivered.get(n.get('notificationId'))
    if not d or d.get('sourceEventId') != n.get('sourceEventId'):
        raise SystemExit('STEP46B: FAIL: sink notification is not bound to a durable delivered decision')
print(f'STEP46B: PASS: {len(source)} lifecycle decisions and {len(notifications)} sink notifications are unique')
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 46B — INDEPENDENT ALERT NOTIFIER"
echo "============================================================"
echo "Durable lifecycle consumer with restart replay, deduplication, cooldown"
echo "and severity escalation. Step 46B has TEST_FILE output only and no network."
echo

echo "[1/9] Step 46A precondition remains valid"
"$ROOT/scripts/step46a-alert-acknowledgement.sh" >/dev/null || fail "Step 46A acknowledgement gate failed"
pass "Step 46A remains valid; ACK is still non-resolving and routing remains disabled"

echo "[2/9] Go regression + notifier policy tests"
if command -v go >/dev/null 2>&1; then
  (
    cd "$ROOT/dashboard-api"
    CGO_ENABLED=0 go test ./... >/dev/null
    CGO_ENABLED=0 go vet ./... >/dev/null
    CGO_ENABLED=0 go build ./... >/dev/null
  ) || fail "Go test/vet/build failed"
elif command -v docker >/dev/null 2>&1; then
  STEP46B_GO_IMAGE=${STEP46B_GO_IMAGE:-control-dashboard-step46b-go:local}
  docker build --target build -t "$STEP46B_GO_IMAGE" "$ROOT/dashboard-api" >/dev/null || fail "could not build local Go regression image"
  docker run --rm "$STEP46B_GO_IMAGE" sh -ec 'cd /src && CGO_ENABLED=0 go test ./... >/dev/null && CGO_ENABLED=0 go vet ./... >/dev/null && CGO_ENABLED=0 go build ./... >/dev/null' || fail "Go test/vet/build failed inside local build-stage image"
else
  fail "neither Go nor Docker is available for the Go regression gate"
fi
pass "bootstrap, deduplication, cooldown, escalation, restart and sink-idempotency regressions pass"

echo "[3/9] Independent notifier container is running and initialized"
command -v docker >/dev/null 2>&1 || fail "Docker is required for the Step 46B runtime gate"
docker inspect control-dashboard-alert-notifier >/dev/null 2>&1 || fail "control-dashboard-alert-notifier container is missing; rebuild the real dashboard stack"
[ "$(docker inspect -f '{{.State.Running}}' control-dashboard-alert-notifier 2>/dev/null)" = "true" ] || fail "alert notifier container is not running"
ATTEMPT=1
while :; do
  if docker exec control-dashboard-alert-notifier sh -ec 'test -s /data/notifier/status.json && grep -q '"'"'"lastSuccessAt"'"'"' /data/notifier/status.json' >/dev/null 2>&1; then
    break
  fi
  [ "$ATTEMPT" -lt 12 ] || fail "notifier did not publish a successful status heartbeat"
  ATTEMPT=$((ATTEMPT + 1)); sleep 2
done
docker exec control-dashboard-alert-notifier cat /data/notifier/status.json >"$STATUS" || fail "could not read notifier status"
python3 - "$STATUS" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('version') != 'step46b-v1': raise SystemExit('STEP46B: FAIL: notifier status version mismatch')
if d.get('bootstrapComplete') is not True: raise SystemExit('STEP46B: FAIL: notifier bootstrap is incomplete')
if d.get('sink') != 'TEST_FILE': raise SystemExit('STEP46B: FAIL: Step 46B sink is not TEST_FILE')
if d.get('minSeverity') not in ('INFO','WARN','CRITICAL'): raise SystemExit('STEP46B: FAIL: invalid minimum severity')
if not str(d.get('lastSuccessAt') or '').strip(): raise SystemExit('STEP46B: FAIL: notifier has no successful sweep')
if d.get('lastError'): raise SystemExit('STEP46B: FAIL: notifier status has lastError: '+str(d.get('lastError')))
print('STEP46B: PASS: notifier heartbeat is initialized and bootstrap-complete')
PY

echo "[4/9] Container boundary is read-only-source / isolated-state / no-network"
docker inspect control-dashboard-alert-notifier >"$BODY" || fail "could not inspect notifier container"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))[0]
if d.get('HostConfig',{}).get('NetworkMode') != 'none':
    raise SystemExit('STEP46B: FAIL: notifier must have network_mode=none')
mounts={m.get('Destination'):m for m in d.get('Mounts',[])}
watch=mounts.get('/data/watchdog'); state=mounts.get('/data/notifier')
if not watch or watch.get('RW') is not False:
    raise SystemExit('STEP46B: FAIL: /data/watchdog must be mounted read-only')
if not state or state.get('RW') is not True:
    raise SystemExit('STEP46B: FAIL: /data/notifier must be the dedicated writable volume')
env=d.get('Config',{}).get('Env',[])
for prefix in ('DASHBOARD_POSTGRES_DSN=','DASHBOARD_NATS_URL=','DASHBOARD_NATS_MONITOR_URL=','PRIVATE_KEY=','MNEMONIC=','SEED_PHRASE='):
    if any(x.startswith(prefix) for x in env):
        raise SystemExit('STEP46B: FAIL: notifier received forbidden env '+prefix[:-1])
if not any(x == 'DASHBOARD_NOTIFIER_SINK=TEST_FILE' for x in env):
    raise SystemExit('STEP46B: FAIL: TEST_FILE sink is not explicit')
print('STEP46B: PASS: notifier has no network/trading dependencies and source is read-only')
PY

echo "[5/9] Durable decisions and TEST_FILE output are lifecycle-deduplicated"
read_notifier_file /data/notifier/events.jsonl "$BEFORE_DECISIONS"
read_notifier_file /data/notifier/test-sink.jsonl "$BEFORE_SINK"
assert_unique_contracts "$BEFORE_DECISIONS" "$BEFORE_SINK"
pass "durable notifier state and test sink are internally consistent"

echo "[6/9] Restart replays state without duplicating prior notifications"
docker restart control-dashboard-alert-notifier >/dev/null || fail "could not restart notifier container"
ATTEMPT=1
while :; do
  if [ "$(docker inspect -f '{{.State.Running}}' control-dashboard-alert-notifier 2>/dev/null || true)" = "true" ] && docker exec control-dashboard-alert-notifier sh -ec 'test -s /data/notifier/status.json' >/dev/null 2>&1; then
    sleep 6
    break
  fi
  [ "$ATTEMPT" -lt 20 ] || fail "notifier did not recover after restart"
  ATTEMPT=$((ATTEMPT + 1)); sleep 1
done
read_notifier_file /data/notifier/events.jsonl "$AFTER_DECISIONS"
read_notifier_file /data/notifier/test-sink.jsonl "$AFTER_SINK"
assert_unique_contracts "$AFTER_DECISIONS" "$AFTER_SINK"
python3 - "$BEFORE_DECISIONS" "$AFTER_DECISIONS" "$BEFORE_SINK" "$AFTER_SINK" <<'PY' || exit 1
import json, sys

def load(path):
    out=[]
    with open(path, encoding='utf-8') as f:
        for line in f:
            line=line.strip()
            if line: out.append(json.loads(line))
    return out
before_d=load(sys.argv[1]); after_d=load(sys.argv[2]); before_s=load(sys.argv[3]); after_s=load(sys.argv[4])
after_source=[d.get('sourceEventId') for d in after_d if d.get('sourceEventId')]
after_notify=[n.get('notificationId') for n in after_s if n.get('notificationId')]
for d in before_d:
    sid=d.get('sourceEventId')
    if sid and after_source.count(sid) != 1:
        raise SystemExit('STEP46B: FAIL: restart duplicated/lost prior decision '+sid)
for n in before_s:
    nid=n.get('notificationId')
    if nid and after_notify.count(nid) != 1:
        raise SystemExit('STEP46B: FAIL: restart duplicated/lost prior notification '+nid)
print('STEP46B: PASS: every pre-restart decision/notification remains exactly once')
PY

echo "[7/9] Static safety: no trading command, exchange or secret surface"
if grep -RInE '\.Publish\(|Publish\(|/exchange|submit.*order|cancel.*order|PRIVATE_KEY|MNEMONIC|SEED_PHRASE|DASHBOARD_POSTGRES_DSN|DASHBOARD_NATS_URL' "$ROOT/dashboard-api/internal/notifier" "$ROOT/dashboard-api/cmd/dashboard-alert-notifier" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "notifier source contains forbidden trading/publish/secret surface"
fi
grep -q 'network_mode: "none"' "$ROOT/docker-compose.real.yml" || fail "local real compose does not isolate notifier network"
grep -q 'dashboard-watchdog-data:/data/watchdog:ro' "$ROOT/docker-compose.real.yml" || fail "notifier source volume is not read-only"
grep -q 'dashboard-alert-notifier-data:/data/notifier' "$ROOT/docker-compose.real.yml" || fail "dedicated notifier state volume missing"
pass "notifier is observability-only and cannot reach trading/exchange networks"

echo "[8/9] Telegram remains impossible in Step 46B"
grep -q 'Step 46B only permits DASHBOARD_NOTIFIER_SINK=TEST_FILE' "$ROOT/dashboard-api/cmd/dashboard-alert-notifier/main.go" || fail "Step 46B sink fail-closed guard missing"
if grep -RInE 'api\.telegram\.org|TELEGRAM_BOT_TOKEN|TELEGRAM_CHAT_ID' "$ROOT/dashboard-api/internal/notifier" "$ROOT/dashboard-api/cmd/dashboard-alert-notifier" "$ROOT/docker-compose.real.yml" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "Telegram surface appeared before Step 46C"
fi
pass "TEST_FILE is the only notifier delivery adapter; Telegram remains Step 46C"

echo "[9/9] Notifier does not authorize private/manual/LIVE trading"
login
READINESS=$(curl -fsS -b "$COOKIE" "$BASE_URL/api/global-readiness") || fail "global readiness unavailable"
printf '%s' "$READINESS" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('privateTestnetReady') is not False or d.get('tradingReady') is not False or d.get('liveReady') is not False:
    raise SystemExit('STEP46B: FAIL: notifier altered private/trading/LIVE readiness')
if d.get('manualRouting') != 'DISABLED':
    raise SystemExit('STEP46B: FAIL: notifier altered manual routing')
print('STEP46B: PASS: private/trading/LIVE remain false and manual routing remains disabled')
PY

echo
echo "============================================================"
echo "STEP 46B: PASS — INDEPENDENT ALERT NOTIFIER VALIDATED"
echo "============================================================"
echo "Notifier state is durable, lifecycle-deduplicated and restart-safe."
echo "Cooldown/escalation policy is covered by regression tests; the runtime"
echo "container has no network and only the idempotent TEST_FILE sink."
echo "Next safe step: Step 46C Telegram Notification Adapter."
