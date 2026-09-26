#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step36-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step36-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP36: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP36: PASS: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 36 — PUBLIC VENUE TRADING RULES"
echo "============================================================"
echo "Public Hyperliquid TESTNET rules only for Step 35-supported mappings."
echo "NO private auth, signing, submit/cancel or capital movement."
echo

echo "[1/7] Step 35 explicit classification precondition"
"$ROOT/scripts/step35-symbol-mapping.sh" >/dev/null || fail "Step 35 explicit symbol classification gate failed"
pass "Step 35 classification remains valid"

echo "[2/7] Public rules adapter safety boundary"
RULE_CLIENT="$ROOT/dashboard-api/internal/integration/hyperliquid/public_rules.go"
[ -f "$RULE_CLIENT" ] || fail "public trading-rules client is missing"
if grep -REn 'Authorization|private[_-]?key|secret[_-]?key|sign(ed|ing)?|submit.*order|cancel.*order|/exchange' "$RULE_CLIENT" >/dev/null 2>&1; then
  fail "public trading-rules adapter contains a private-auth/order surface"
fi
grep -q '"type": "meta"' "$RULE_CLIENT" || fail "public rules adapter does not use fixed meta query"
grep -q '"type": "allMids"' "$RULE_CLIENT" || fail "public rules adapter does not use fixed allMids query"
pass "rules adapter is public /info metadata+mids only"

echo "[3/7] Protocol precision constants are explicit"
PROVIDER_RULES="$ROOT/dashboard-api/internal/provider/venue_rules.go"
[ -f "$PROVIDER_RULES" ] || fail "venue rules provider is missing"
grep -q 'hyperliquidPerpPriceMaxSignificantFigures = 5' "$PROVIDER_RULES" || fail "5-significant-figure price rule is not explicit"
grep -q 'hyperliquidPerpMaxDecimals.*= 6' "$PROVIDER_RULES" || fail "perp MAX_DECIMALS=6 rule is not explicit"
grep -q 'hyperliquidMinOrderNotionalUSD.*= 10.0' "$PROVIDER_RULES" || fail "10 USD minimum order-notional rule is not explicit"
pass "price/size/min-notional rules are explicit and unit-tested"

echo "[4/7] Authenticated current supported-universe rule validation"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login") \
  || fail "cannot reach dashboard login at $BASE_URL"
[ "$CODE" = "200" ] || { cat "$BODY" >&2; fail "dashboard login returned HTTP $CODE"; }

RULES=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/venue-rules") || fail "venue-rules endpoint unavailable"
printf '%s' "$RULES" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, math, sys
p=sys.argv[1]
d=json.load(open(p, encoding='utf-8'))
if d.get('status') != 'VALIDATED' or d.get('validated') is not True:
    print(json.dumps(d, indent=2, ensure_ascii=False), file=sys.stderr)
    raise SystemExit('STEP36: FAIL: public trading rules are not fully validated for every Step 35-supported mapping')
if d.get('venue') != 'HYPERLIQUID' or d.get('targetEnvironment') != 'TESTNET':
    raise SystemExit('STEP36: FAIL: venue/environment identity mismatch')
if d.get('privateAuth') != 'DISABLED' or d.get('orderRouting') != 'DISABLED' or d.get('readOnly') is not True:
    raise SystemExit('STEP36: FAIL: Step 36 crossed the public/read-only boundary')
supported=int(d.get('supportedMappingCount') or 0)
validated=int(d.get('validatedRuleCount') or 0)
blocked=int(d.get('blockedRuleCount') or 0)
nonroutable=int(d.get('nonRoutableMappingCount') or 0)
if supported <= 0 or validated != supported or blocked != 0:
    raise SystemExit(f'STEP36: FAIL: rule coverage invalid: validated={validated}/{supported}, blocked={blocked}')
if int(d.get('priceMaxSignificantFigures') or 0) != 5 or int(d.get('perpMaxDecimals') or 0) != 6 or d.get('integerPricesAlwaysAllowed') is not True:
    raise SystemExit('STEP36: FAIL: protocol price precision constants/exception mismatch')
if abs(float(d.get('minOrderNotionalUsd') or 0)-10.0) > 1e-12:
    raise SystemExit('STEP36: FAIL: minimum order notional is not 10 USD')
rows=d.get('rows') or []
if len(rows) != supported:
    raise SystemExit(f'STEP36: FAIL: rows={len(rows)} but supportedMappingCount={supported}')
for r in rows:
    if r.get('state') != 'VALID':
        raise SystemExit(f"STEP36: FAIL: {r.get('internal')} rule state={r.get('state')}: {r.get('reason')}")
    sd=int(r.get('sizeDecimals'))
    if sd < 0 or sd > 6:
        raise SystemExit(f"STEP36: FAIL: {r.get('internal')} invalid szDecimals={sd}")
    if int(r.get('priceMaxSignificantFigures') or 0) != 5 or int(r.get('priceMaxDecimals')) != 6-sd:
        raise SystemExit(f"STEP36: FAIL: {r.get('internal')} derived price precision mismatch")
    if int(r.get('maxLeverage') or 0) <= 0:
        raise SystemExit(f"STEP36: FAIL: {r.get('internal')} maxLeverage is not positive")
    if r.get('isDelisted') is True:
        raise SystemExit(f"STEP36: FAIL: {r.get('internal')} is delisted")
    try:
        mid=float(r.get('midPrice'))
        est=float(r.get('estimatedMinOrderNotionalUsd'))
    except Exception:
        raise SystemExit(f"STEP36: FAIL: {r.get('internal')} has invalid public mid/min-order estimate")
    if not math.isfinite(mid) or mid <= 0 or not math.isfinite(est) or est + 1e-9 < 10.0:
        raise SystemExit(f"STEP36: FAIL: {r.get('internal')} public rule estimate is unsafe: mid={mid} notional={est}")
print(f"STEP36: PASS: public rules validated {validated}/{supported}; Step35 non-routable={nonroutable}; minNotional=${d['minOrderNotionalUsd']:.2f}; metadataLatencyMs={d.get('metadataLatencyMs')}")
for r in rows:
    margin='ISOLATED' if r.get('onlyIsolated') else 'DEFAULT'
    print(f"STEP36: RULE: {r['internal']} -> {r['venue']} szDecimals={r['sizeDecimals']} sizeStep={r['sizeStepLabel']} price<=5sig/<= {r['priceMaxDecimals']}dp maxLev={r['maxLeverage']}x margin={margin} mid={r['midPrice']} estMinSize={r['estimatedMinOrderSizeLabel']}")
PY

echo "[5/7] Unsupported Step 35 assets remain excluded from rule/routing coverage"
MAP=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/venue-symbol-map") || fail "venue-symbol-map endpoint unavailable"
printf '%s' "$MAP" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if not d.get('validated'):
    raise SystemExit('STEP36: FAIL: Step 35 mapping endpoint no longer validates')
unsupported=int(d.get('unsupportedRequiredCount') or 0)
if unsupported != int(d.get('requiredSymbolCount') or 0)-int(d.get('mappedRequiredCount') or 0):
    raise SystemExit('STEP36: FAIL: Step 35 unsupported coverage arithmetic mismatch')
print(f'STEP36: PASS: {unsupported} unsupported/venue-absent symbols remain explicitly non-routable')
PY

echo "[6/7] No exchange secret or command surface"
if grep -REn 'DASHBOARD_(VENUE|EXCHANGE)_[A-Z0-9_]*(API_KEY|SECRET|PRIVATE_KEY|SIGNING_KEY)' \
  "$ROOT/docker-compose.yml" "$ROOT/docker-compose.real.yml" "$ROOT/docker-compose.production.yml" "$ROOT/docker-compose.production.real.yml" "$ROOT/dashboard-api" \
  --include='*.go' --include='*.yml' >"$BODY"; then
  cat "$BODY" >&2
  fail "exchange-secret configuration was introduced during Step 36"
fi
ROUTES=$(sed -n '/func (s \*Server) routes()/,/return s.middleware/p' "$ROOT/dashboard-api/internal/server/server.go")
printf '%s\n' "$ROUTES" | grep -Eq '/api/(submit|cancel|pause|resume|kill|order)' && fail "dangerous trading route found"
pass "no private venue credential or trading command surface exists"

echo "[7/7] Manual route remains disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP36: FAIL: manual-control routeEnabled=true')
print('STEP36: PASS: manual routing remains fail-closed')
PY

echo
echo "============================================================"
echo "STEP 36: PASS — PUBLIC VENUE TRADING RULES VALIDATED"
echo "============================================================"
echo "Precision, leverage and minimum-notional diagnostics are available for"
echo "every Step 35-supported Hyperliquid TESTNET mapping. Unsupported assets"
echo "remain non-routable. No private auth, signing or order path is enabled."
echo "Next: Step 37 may add TESTNET PRIVATE AUTH only."
echo "No real capital is required."
