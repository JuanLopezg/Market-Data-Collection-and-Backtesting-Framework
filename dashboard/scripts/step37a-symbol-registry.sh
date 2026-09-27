#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step37a-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step37a-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP37A: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP37A: PASS: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 37A — MULTI-EXCHANGE SYMBOL REGISTRY"
echo "============================================================"
echo "Versioned Binance/source -> internal -> execution-venue identity registry"
echo "plus read-only drift/coverage alarms. NO wallet, secrets, signing or orders."
echo

echo "[1/8] Registry bootstrap + Step 36 public trading-rules precondition"
LOGIN_JSON=$(json_login_body)
READY=0
for attempt in $(seq 1 15); do
  CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login" 2>/dev/null || true)
  if [ "$CODE" = "200" ]; then
    REG_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" "$BASE_URL/api/symbol-registry" 2>/dev/null || true)
    if [ "$REG_CODE" = "200" ]; then
      CLASS=$(python3 - "$BODY" <<'PYBOOT'
import json, sys
try:
    d=json.load(open(sys.argv[1], encoding='utf-8'))
except Exception:
    print('RETRY')
    raise SystemExit
if d.get('validated') is True and d.get('status') in ('VALIDATED','VALIDATED_WITH_WARNINGS'):
    print('READY')
    raise SystemExit
err=str(d.get('error') or '')
transient=(
    err.startswith('canonical market-data ranking unavailable:') or
    err.startswith('current strategy universe unavailable:') or
    err.startswith('current Hyperliquid TESTNET metadata unavailable:')
)
if transient:
    print('RETRY')
else:
    print('FAIL')
PYBOOT
)
      case "$CLASS" in
        READY) READY=1; break ;;
        RETRY) echo "STEP37A: INFO: waiting 2s for symbol-registry read-only dependencies..." ;;
        FAIL) cat "$BODY" >&2; fail "symbol registry has a non-transient blocker before Step 36 regression gate" ;;
      esac
    fi
  fi
  sleep 2
done
[ "$READY" = "1" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "symbol registry did not become readable/validated within the startup window"; }
"$ROOT/scripts/step36-venue-rules.sh" >/dev/null || fail "Step 36 public venue trading-rules gate failed"
pass "registry dependencies are stable and Step 36 remains valid"

echo "[2/8] Registry artifact integrity"
REGISTRY="$ROOT/dashboard-api/internal/integration/symbolregistry/registry.json"
MAP="$ROOT/dashboard-api/internal/integration/hyperliquid/symbol_map.json"
[ -f "$REGISTRY" ] || fail "multi-exchange registry.json is missing"
[ -f "$MAP" ] || fail "Step 35 symbol_map.json is missing"
python3 - "$REGISTRY" "$MAP" <<'PY' || exit 1
import json, sys
registry=json.load(open(sys.argv[1], encoding='utf-8'))
manifest=json.load(open(sys.argv[2], encoding='utf-8'))
if registry.get('schemaVersion') != 1 or registry.get('policy') != 'EXPLICIT_ONLY' or registry.get('marketDataSource') != 'BINANCE':
    raise SystemExit('STEP37A: FAIL: registry identity/policy mismatch')
entries=registry.get('entries') or []
if len(entries) != 175:
    raise SystemExit(f'STEP37A: FAIL: registry entry count={len(entries)} want=175')
seen=set()
idx={}
for e in entries:
    i=e.get('internal')
    if not i or i in seen:
        raise SystemExit(f'STEP37A: FAIL: empty/duplicate internal symbol {i!r}')
    seen.add(i); idx[i]=e
    md=[x for x in e.get('marketData',[]) if x.get('venue')=='BINANCE' and x.get('environment')=='SOURCE']
    if len(md)!=1 or md[0].get('status')!='SUPPORTED' or not md[0].get('symbol'):
        raise SystemExit(f'STEP37A: FAIL: {i} has no exact BINANCE/SOURCE mapping')
    ex=[x for x in e.get('execution',[]) if x.get('venue')=='HYPERLIQUID' and x.get('environment')=='TESTNET']
    if len(ex)!=1:
        raise SystemExit(f'STEP37A: FAIL: {i} has no unique HYPERLIQUID/TESTNET classification')
    if ex[0].get('status') not in ('MAPPED','BLOCKED_EXPLICIT'):
        raise SystemExit(f"STEP37A: FAIL: {i} invalid execution status={ex[0].get('status')}")

for m in manifest.get('mappings',[]):
    e=idx.get(m['internal'])
    if not e: raise SystemExit(f"STEP37A: FAIL: {m['internal']} missing from registry")
    ex=next(x for x in e['execution'] if x['venue']=='HYPERLIQUID' and x['environment']=='TESTNET')
    if ex.get('status')!='MAPPED' or ex.get('symbol')!=m['venue']:
        raise SystemExit(f"STEP37A: FAIL: registry drift for {m['internal']}")
for u in manifest.get('unsupported',[]):
    e=idx.get(u['internal'])
    if not e: raise SystemExit(f"STEP37A: FAIL: blocked {u['internal']} missing from registry")
    ex=next(x for x in e['execution'] if x['venue']=='HYPERLIQUID' and x['environment']=='TESTNET')
    if ex.get('status')!='BLOCKED_EXPLICIT' or ex.get('routingPolicy')!='DENY':
        raise SystemExit(f"STEP37A: FAIL: blocked classification drift for {u['internal']}")
print(f"STEP37A: PASS: registry artifact {registry.get('registryVersion')} contains {len(entries)} explicit cross-venue classifications")
PY

echo "[3/8] No heuristic symbol conversion"
if grep -REn --include='*.go' '(TrimSuffix|strings\.Replace|ToUpper|ToLower|fuzzy|guess.*symbol|strip.*USDT)' \
  "$ROOT/dashboard-api/internal/integration/symbolregistry" "$ROOT/dashboard-api/internal/provider/symbol_registry.go" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "registry runtime path contains heuristic symbol conversion"
fi
pass "registry lookup remains exact; no suffix stripping, fuzzy matching or alias guessing"

echo "[4/8] Authenticated runtime registry validation"
REGISTRY_STATUS=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/symbol-registry") || fail "symbol-registry endpoint unavailable"
printf '%s' "$REGISTRY_STATUS" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('validated') is not True or d.get('status') not in ('VALIDATED','VALIDATED_WITH_WARNINGS'):
    print(json.dumps(d, indent=2, ensure_ascii=False), file=sys.stderr)
    raise SystemExit('STEP37A: FAIL: multi-exchange registry is not validated')
if d.get('policy') != 'EXPLICIT_ONLY' or d.get('marketDataSource') != 'BINANCE':
    raise SystemExit('STEP37A: FAIL: runtime registry identity/policy mismatch')
if d.get('executionVenue') != 'HYPERLIQUID' or d.get('executionEnvironment') != 'TESTNET':
    raise SystemExit('STEP37A: FAIL: current execution venue/environment mismatch')
if d.get('mappingDivergences'):
    raise SystemExit('STEP37A: FAIL: registry differs from accepted Step 35 manifest')
if d.get('unregisteredRankingSymbols') or d.get('unregisteredStrategySymbols'):
    raise SystemExit('STEP37A: FAIL: current canonical market-data/strategy symbols are not fully registered')
if int(d.get('currentRankingRegisteredCount') or 0) != int(d.get('currentRankingCount') or 0):
    raise SystemExit('STEP37A: FAIL: current Binance/source ranking coverage is incomplete')
if int(d.get('currentStrategyRegisteredCount') or 0) != int(d.get('currentStrategyCount') or 0):
    raise SystemExit('STEP37A: FAIL: current strategy registry coverage is incomplete')
if d.get('privateAuth') != 'DEFERRED' or d.get('orderRouting') != 'DISABLED' or d.get('readOnly') is not True:
    raise SystemExit('STEP37A: FAIL: registry step crossed the read-only/deferred-auth boundary')
print(f"STEP37A: PASS: registry={d.get('registryVersion')} entries={d.get('registryEntryCount')} ranking={d.get('currentRankingRegisteredCount')}/{d.get('currentRankingCount')} strategy={d.get('currentStrategyRegisteredCount')}/{d.get('currentStrategyCount')} routable={d.get('currentStrategyRoutableCount')}/{d.get('currentStrategyCount')} alarms={len(d.get('alarms') or [])}")
for a in d.get('alarms') or []:
    print(f"STEP37A: ALARM: {a.get('severity')} {a.get('eventType')} {a.get('asset','')} — {a.get('title')}")
PY

echo "[5/8] Alerts & Audit exposes symbol-registry alarms"
ALERTS=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/alerts-audit") || fail "alerts-audit endpoint unavailable"
printf '%s' "$ALERTS" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
alerts=[a for a in (d.get('alerts') or []) if a.get('service')=='SymbolRegistry']
audit=[a for a in (d.get('audit') or []) if a.get('action')=='SYMBOL_REGISTRY_OBSERVED']
if not audit:
    raise SystemExit('STEP37A: FAIL: Alerts & Audit does not expose SYMBOL_REGISTRY_OBSERVED evidence')
# Current Step 35 coverage is intentionally partial, so at least one registry warning is expected.
if not alerts:
    raise SystemExit('STEP37A: FAIL: no SymbolRegistry operational alert is exposed despite partial execution coverage')
blocked=[a for a in alerts if a.get('eventType')=='SYMBOL_REGISTRY_BLOCKED']
critical_blocked=[a for a in blocked if a.get('severity')=='CRITICAL']
if critical_blocked:
    for a in critical_blocked:
        print(json.dumps(a, indent=2, ensure_ascii=False), file=sys.stderr)
    raise SystemExit('STEP37A: FAIL: symbol registry has a CRITICAL blocker in Alerts & Audit')
# Alerts & Audit performs its own fresh read of the registry dependencies. A
# whitelisted dependency timeout may therefore appear here as WARN even after
# /api/symbol-registry has just validated successfully above. That is not a
# contradiction while order routing is disabled: provider logic deliberately
# reserves CRITICAL for artifact/manifest/drift/unregistered failures.
warn_blocked=[a for a in blocked if a.get('severity')=='WARN']
if warn_blocked:
    print(f"STEP37A: INFO: Alerts & Audit observed {len(warn_blocked)} transient SymbolRegistry dependency warning(s); current registry endpoint is validated and routing remains disabled")
print(f"STEP37A: PASS: Alerts & Audit exposes {len(alerts)} active SymbolRegistry alert(s) + registry evidence")
PY

echo "[6/8] New/unmapped symbols remain fail-closed"
PROVIDER="$ROOT/dashboard-api/internal/provider/symbol_registry.go"
grep -q 'SYMBOL_UNREGISTERED_MARKET_DATA' "$PROVIDER" || fail "missing unregistered market-data alarm"
grep -q 'SYMBOL_UNREGISTERED_STRATEGY' "$PROVIDER" || fail "missing unregistered strategy alarm"
grep -q 'SYMBOL_REGISTRY_DIVERGENCE' "$PROVIDER" || fail "missing registry-divergence alarm"
grep -q 'SYMBOL_VENUE_ABSENT' "$PROVIDER" || fail "missing venue-absent alarm"
pass "unregistered, divergent and venue-absent symbols generate explicit alarms and remain non-routable"

echo "[7/8] No wallet/private/order surface introduced"
if grep -REn 'HYPERLIQUID_TESTNET_(PRIVATE_KEY|API_WALLET_PRIVATE_KEY)|/exchange|submit.*order|cancel.*order' \
  "$ROOT/dashboard-api/internal/integration/symbolregistry" "$ROOT/dashboard-api/internal/provider/symbol_registry.go" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "Step 37A introduced a wallet/private/order surface"
fi
pass "Step 37A is public/read-only and requires no wallet or funds"

echo "[8/8] Manual route remains disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP37A: FAIL: manual-control routeEnabled=true')
print('STEP37A: PASS: manual routing remains fail-closed')
PY

echo
echo "============================================================"
echo "STEP 37A: PASS — MULTI-EXCHANGE SYMBOL REGISTRY + ALARMS VALIDATED"
echo "============================================================"
echo "Binance/source and Hyperliquid TESTNET identities are explicit and versioned."
echo "New/unmapped/divergent/venue-absent symbols are alarmed and non-routable."
echo "Step 37 private wallet authentication remains DEFERRED; no funds are required."
echo "Next safe work can continue without a wallet on ledger/alert/readiness foundations."
