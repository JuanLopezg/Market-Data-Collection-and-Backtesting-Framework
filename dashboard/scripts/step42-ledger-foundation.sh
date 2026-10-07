#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step42-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step42-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

fail() { echo "STEP42: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP42: PASS: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 42 — APPEND-ONLY LEDGER FOUNDATION"
echo "============================================================"
echo "Read-only deterministic economic projection over append-only trading_fills."
echo "NO wallet, private auth, signing, submit/cancel or capital movement."
echo

echo "[1/7] Step 37A symbol-registry precondition"
"$ROOT/scripts/step37a-symbol-registry.sh" >/dev/null || fail "Step 37A multi-exchange symbol-registry gate failed"
pass "Step 37A remains valid; private Step 37 stays deferred"

echo "[2/7] Append-only source + bounded ledger query contract"
grep -q '^[- ]*`trading_fills`' "$ROOT/docs/REAL_DATA_SOURCE_MAP.md" || fail "verified trading_fills source contract is missing"
grep -q 'append-only by `fill_id`' "$ROOT/docs/REAL_DATA_SOURCE_MAP.md" || fail "verified append-only fill_id contract is missing"
grep -q 'func (r \*Reader) LedgerFills' "$ROOT/dashboard-api/internal/integration/postgres/reader.go" || fail "bounded LedgerFills reader is missing"
grep -q 'count(DISTINCT fill_id)' "$ROOT/dashboard-api/internal/integration/postgres/reader.go" || fail "fill_id uniqueness diagnostic is missing"
grep -q 'LIMIT %d' "$ROOT/dashboard-api/internal/integration/postgres/reader.go" || fail "ledger detail query is not bounded"
pass "ledger source is append-only by audited runtime contract; read detail remains bounded"

echo "[3/7] Deterministic economic-entry invariants"
grep -q 'EntryID:.*"fill:"' "$ROOT/dashboard-api/internal/provider/ledger.go" || fail "fill-derived immutable entry IDs are missing"
grep -q 'sha256.Sum256' "$ROOT/dashboard-api/internal/provider/ledger.go" || fail "deterministic recent-window fingerprint is missing"
grep -q 'DurableRealizedPnL:.*false' "$ROOT/dashboard-api/internal/provider/ledger.go" || fail "Step 42 must not claim durable realized PnL"
grep -q 'HistoricalEquityAvailable:.*false' "$ROOT/dashboard-api/internal/provider/ledger.go" || fail "Step 42 must not claim historical equity"
pass "fill -> position/cash deltas are deterministic and unsupported PnL/equity claims remain disabled"

echo "[4/7] Authenticated runtime ledger validation"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login" 2>/dev/null || true)
[ "$CODE" = "200" ] || { cat "$BODY" >&2 2>/dev/null || true; fail "dashboard login failed with HTTP $CODE"; }
LEDGER=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/ledger") || fail "ledger endpoint unavailable"
printf '%s' "$LEDGER" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, math, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('validated') is not True or d.get('foundationReady') is not True or d.get('status') != 'VALIDATED':
    print(json.dumps(d, indent=2, ensure_ascii=False), file=sys.stderr)
    raise SystemExit('STEP42: FAIL: ledger foundation is not validated')
rows=int(d.get('totalFillRows') or 0)
distinct=int(d.get('distinctFillIds') or 0)
invalid=int(d.get('invalidFillRows') or 0)
if rows != distinct or invalid != 0:
    raise SystemExit(f'STEP42: FAIL: ledger integrity mismatch rows={rows} distinct={distinct} invalid={invalid}')
for key in ('totalFees','grossBuyNotional','grossSellNotional','netCashDeltaFromFills'):
    v=float(d.get(key) or 0)
    if not math.isfinite(v):
        raise SystemExit(f'STEP42: FAIL: non-finite {key}')
if d.get('appendOnlySource') is not True or d.get('deterministicProjection') is not True or d.get('readOnly') is not True:
    raise SystemExit('STEP42: FAIL: ledger read-only/append-only contract mismatch')
if d.get('durableRealizedPnl') is not False or d.get('durableUnrealizedPnl') is not False or d.get('historicalEquityAvailable') is not False:
    raise SystemExit('STEP42: FAIL: Step 42 is claiming accounting outputs it does not durably own')
if d.get('privateAuth') != 'DEFERRED' or d.get('orderRouting') != 'DISABLED':
    raise SystemExit('STEP42: FAIL: Step 42 crossed the deferred-auth/no-routing boundary')
entries=d.get('entries') or []
ids=[e.get('entryId') for e in entries]
if len(ids) != len(set(ids)):
    raise SystemExit('STEP42: FAIL: recent ledger entry IDs are not unique')
for e in entries:
    if not str(e.get('entryId','')).startswith('fill:') or len(str(e.get('entryHash',''))) != 64:
        raise SystemExit('STEP42: FAIL: malformed deterministic ledger entry identity/hash')
print(f"STEP42: PASS: ledger validated rows={rows} recent={len(entries)} fees={d.get('totalFeesLabel')} fingerprint={str(d.get('recentWindowFingerprint',''))[:16]}…")
PY

echo "[5/7] Ledger endpoint is authenticated GET-only"
POST_CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -b "$COOKIE_JAR" -X POST "$BASE_URL/api/ledger" 2>/dev/null || true)
[ "$POST_CODE" = "405" ] || fail "POST /api/ledger returned HTTP $POST_CODE; expected 405"
pass "ledger endpoint is authenticated and exposes no mutation method"

echo "[6/7] No SQL writes, wallet, signing or order command surface"
if grep -REn --include='*.go' '\b(INSERT|UPDATE|DELETE|ALTER|DROP|TRUNCATE)\b' \
  "$ROOT/dashboard-api/internal/provider/ledger.go" "$ROOT/dashboard-api/internal/integration/postgres/reader.go" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "Step 42 ledger path contains a SQL mutation keyword"
fi
if grep -REn 'PRIVATE_KEY|seed phrase|mnemonic|/exchange|submit.*order|cancel.*order' \
  "$ROOT/dashboard-api/internal/provider/ledger.go" >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "Step 42 introduced a wallet/signing/order surface"
fi
pass "ledger path remains read-only and wallet/order independent"

echo "[7/7] Manual route remains disabled"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP42: FAIL: manual-control routeEnabled=true')
print('STEP42: PASS: manual routing remains fail-closed')
PY

echo
echo "============================================================"
echo "STEP 42: PASS — APPEND-ONLY LEDGER FOUNDATION VALIDATED"
echo "============================================================"
echo "Persisted fills now have a deterministic read-only economic ledger projection."
echo "No durable realized/unrealized PnL or historical equity is claimed yet."
echo "Step 37 private auth and Steps 38-41 remain DEFERRED; no wallet/funds required."
echo "Next safe step: Step 43 durable Alerts / Watchdog foundation."
