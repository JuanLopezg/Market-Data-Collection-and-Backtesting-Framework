#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step35-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step35-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP35: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP35: PASS: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PYLOGIN'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PYLOGIN
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 35.3 — EXPLICIT SYMBOL CLASSIFICATION"
echo "============================================================"
echo "Exact internal -> Hyperliquid TESTNET mapping or explicit UNSUPPORTED classification only."
echo "NO heuristic conversion, secrets, signing or orders. Unsupported assets remain non-routable."
echo

echo "[1/6] Step 34 public venue precondition"
"$ROOT/scripts/step34-public-venue.sh" >/dev/null || fail "Step 34 public venue gate failed"
pass "Step 34 public venue connectivity remains valid"

echo "[2/6] Explicit mapping/classification artifact"
MAP="$ROOT/dashboard-api/internal/integration/hyperliquid/symbol_map.json"
[ -f "$MAP" ] || fail "explicit symbol classification artifact is missing"
python3 - "$MAP" <<'PYMAP' || exit 1
import json, sys
p=sys.argv[1]
d=json.load(open(p, encoding='utf-8'))
if d.get('schemaVersion') != 2 or d.get('venue') != 'HYPERLIQUID' or d.get('environment') != 'TESTNET' or d.get('mappingPolicy') != 'EXPLICIT_ONLY':
    raise SystemExit('STEP35: FAIL: mapping manifest identity/policy is invalid')
mapped=d.get('mappings') or []
unsupported=d.get('unsupported') or []
if not mapped:
    raise SystemExit('STEP35: FAIL: mapping manifest is empty')
all_internal=[r.get('internal') for r in mapped] + [r.get('internal') for r in unsupported]
if len(all_internal) != len(set(all_internal)):
    raise SystemExit('STEP35: FAIL: duplicate internal symbol classification')
for r in unsupported:
    if not r.get('internal') or not r.get('reason'):
        raise SystemExit('STEP35: FAIL: unsupported classification lacks symbol/reason')
print(f'STEP35: PASS: artifact contains {len(mapped)} exact mappings + {len(unsupported)} explicit blocked classifications')
PYMAP

echo "[3/6] No heuristic mapping implementation"
if grep -REn 'TrimSuffix\([^)]*USDT|ReplaceAll\([^)]*USDT|HasSuffix\([^)]*USDT|strings\.ToUpper\([^)]*symbol|strings\.ToLower\([^)]*symbol' \
  "$ROOT/dashboard-api/internal/provider/venue_symbol_mapping.go" \
  "$ROOT/dashboard-api/internal/integration/hyperliquid/symbol_map.go" >/dev/null 2>&1; then
  fail "heuristic symbol conversion detected in Step 35 mapping path"
fi
pass "runtime mapping path performs exact manifest/classification lookup only"

echo "[4/6] Authenticated current-universe classification validation"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login") \
  || fail "cannot reach dashboard login at $BASE_URL"
[ "$CODE" = "200" ] || { cat "$BODY" >&2; fail "dashboard login returned HTTP $CODE"; }

# The canonical market-data SQLite source can briefly be busy immediately after
# startup or while other read-only dashboard resources are being refreshed.
# Keep the endpoint fail-closed, but allow a small bounded retry window ONLY for
# the audited transient SQLite timeout/busy/locked conditions. Any semantic mapping/classification
# blocker still fails immediately.
MAPPING_READY=0
for attempt in 1 2 3 4 5; do
  MAPPING=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/venue-symbol-map") || fail "venue-symbol-map endpoint unavailable"
  printf '%s' "$MAPPING" >"$BODY"
  CLASS=$(python3 - "$BODY" <<'PYRETRY'
import json, sys
try:
    d=json.load(open(sys.argv[1], encoding='utf-8'))
except Exception:
    print('FAIL')
    raise SystemExit
if d.get('validated') is True and d.get('status') in ('VALIDATED', 'VALIDATED_WITH_UNSUPPORTED'):
    print('READY')
    raise SystemExit
err=str(d.get('error') or '')
lower=err.lower()
transient=(
    err.startswith('canonical strategy universe unavailable:') and
    (
        'sqlite query timed out after ' in lower or
        'database is locked' in lower or
        'database is busy' in lower
    )
)
print('RETRY' if transient else 'FAIL')
PYRETRY
  )
  case "$CLASS" in
    READY) MAPPING_READY=1; break ;;
    RETRY)
      if [ "$attempt" -lt 5 ]; then
        echo "STEP35: INFO: canonical strategy SQLite read is transiently busy/locked/timed out; retrying in 2s ($attempt/5)..."
        sleep 2
      fi
      ;;
    FAIL) break ;;
  esac
done
[ "$MAPPING_READY" = "1" ] || {
  cat "$BODY" >&2
  fail "every current canonical strategy symbol must be explicitly mapped or explicitly blocked"
}

python3 - "$BODY" <<'PYGATE' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('status') not in ('VALIDATED', 'VALIDATED_WITH_UNSUPPORTED') or d.get('validated') is not True:
    print(json.dumps(d, indent=2, ensure_ascii=False), file=sys.stderr)
    missing=d.get('missingInternalSymbols') or []
    if missing:
        print('STEP35: UNCLASSIFIED INTERNAL SYMBOLS: ' + ', '.join(missing), file=sys.stderr)
    raise SystemExit('STEP35: FAIL: every current canonical strategy symbol must be explicitly mapped or explicitly blocked')
if d.get('venue') != 'HYPERLIQUID' or d.get('targetEnvironment') != 'TESTNET' or d.get('policy') != 'EXPLICIT_ONLY':
    raise SystemExit('STEP35: FAIL: mapping endpoint identity/policy mismatch')
if d.get('privateAuth') != 'DISABLED' or d.get('orderRouting') != 'DISABLED' or d.get('exchangeFilters') != 'NOT_APPLIED' or d.get('readOnly') is not True:
    raise SystemExit('STEP35: FAIL: mapping validation crossed the Step 35 safety boundary')
req=int(d.get('requiredSymbolCount') or 0)
classified=int(d.get('classifiedRequiredCount') or 0)
supported=int(d.get('mappedRequiredCount') or 0)
unsupported=int(d.get('unsupportedRequiredCount') or 0)
if req <= 0 or classified != req or supported + unsupported != req:
    raise SystemExit(f'STEP35: FAIL: classification coverage invalid: classified={classified}/{req}, supported={supported}, unsupported={unsupported}')
if d.get('missingInternalSymbols'):
    raise SystemExit('STEP35: FAIL: unclassified symbols remain')
print(f"STEP35: PASS: explicit classification {classified}/{req}; supported={supported}; blocked/unsupported={unsupported}; catalog={d.get('classificationEntryCount')} metadataLatencyMs={d.get('metadataLatencyMs')}")
if unsupported:
    names=d.get('unsupportedInternalSymbols') or []
    absent=d.get('missingVenueSymbols') or []
    print('STEP35: INFO: NON-ROUTABLE INTERNAL SYMBOLS: ' + ', '.join(names))
    if absent:
        print('STEP35: INFO: MAPPED VENUE COINS ABSENT FROM CURRENT TESTNET: ' + ', '.join(absent))
    print('STEP35: INFO: executionCoverageComplete=false is expected; these assets remain fail-closed and MUST NOT be routed.')
PYGATE

echo "[5/6] No exchange secrets or trading command surface"
if grep -REn 'private[_-]?key|secret[_-]?key|sign(ed|ing)?|submit.*order|cancel.*order' \
  "$ROOT/dashboard-api/internal/provider/venue_symbol_mapping.go" \
  "$ROOT/dashboard-api/internal/integration/hyperliquid/symbol_map.go" >/dev/null 2>&1; then
  fail "Step 35 mapping code contains a private-auth or order-routing surface"
fi
pass "classification validation is public/read-only and contains no signing/order surface"

echo "[6/6] Manual route remains disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PYMANUAL' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP35: FAIL: manual-control routeEnabled=true')
print('STEP35: PASS: manual routing remains fail-closed')
PYMANUAL

echo
echo "============================================================"
echo "STEP 35: PASS — EXPLICIT SYMBOL CLASSIFICATION VALIDATED"
echo "============================================================"
echo "Every current strategy symbol is explicitly mapped or explicitly blocked."
echo "Unsupported/venue-absent assets remain NON-ROUTABLE; no order path is enabled."
echo "Next: Step 36 may validate public venue trading rules only for supported mappings."
echo "Private auth, signing, submit/cancel and real capital remain out of scope."
