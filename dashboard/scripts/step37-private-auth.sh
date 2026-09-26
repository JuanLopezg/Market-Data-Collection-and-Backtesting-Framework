#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
BASE_URL=${DASHBOARD_BASE_URL:-http://localhost:8080}
USER=${DASHBOARD_GATE_USERNAME:-operator}
PASS=${DASHBOARD_GATE_PASSWORD:-operator-demo}
KEY_FILE=${HYPERLIQUID_TESTNET_API_WALLET_PRIVATE_KEY_FILE:-${HOME}/.config/algotrading/secrets/hyperliquid-testnet-api-wallet.key}
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step37-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step37-body.$$"
TMPDIR37=$(mktemp -d "${TMPDIR:-/tmp}/control-dashboard-step37.XXXXXX")
trap 'rm -f "$COOKIE_JAR" "$BODY"; rm -rf "$TMPDIR37"' EXIT
umask 077

fail() { echo "STEP37: FAIL: $*" >&2; exit 1; }
pass() { echo "STEP37: PASS: $*"; }

json_login_body() {
  python3 - "$USER" "$PASS" <<'PY'
import json, sys
print(json.dumps({"username": sys.argv[1], "password": sys.argv[2]}))
PY
}

echo "============================================================"
echo "CONTROL DASHBOARD STEP 37 — TESTNET API WALLET AUTHORIZATION"
echo "============================================================"
echo "Verifies approved Hyperliquid TESTNET account/API-wallet identity + local key ownership."
echo "NO private key enters dashboard-api. NO /exchange call, order, cancel or capital movement."
echo

echo "[1/8] Step 36 public venue rules precondition"
if ! "$ROOT/scripts/step36-venue-rules.sh" >"$BODY" 2>&1; then
  cat "$BODY" >&2
  fail "Step 36 public venue trading-rules gate failed"
fi
pass "Step 36 public trading rules remain valid"

echo "[2/8] Dashboard secret-isolation boundary"
if grep -REn 'HYPERLIQUID[^[:space:]]*(PRIVATE_KEY|SECRET_KEY)|DASHBOARD[^[:space:]]*(PRIVATE_KEY|SECRET_KEY)' \
  "$ROOT/dashboard-api" "$ROOT/docker-compose.yml" "$ROOT/docker-compose.real.yml" \
  --include='*.go' --include='*.yml' >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "dashboard runtime must not accept or mount Hyperliquid private-key material"
fi
if grep -REn 'api\.hyperliquid-testnet\.xyz/exchange|/exchange' "$ROOT/dashboard-api/internal/integration/hyperliquid" --include='*.go' >"$BODY" 2>/dev/null; then
  cat "$BODY" >&2
  fail "Step 37 dashboard integration must not call Hyperliquid /exchange"
fi
pass "dashboard keeps private key outside its containers and exposes no /exchange client"

echo "[3/8] Authenticated TESTNET account/API-wallet authorization probe"
LOGIN_JSON=$(json_login_body)
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' -c "$COOKIE_JAR" -H 'Content-Type: application/json' --data "$LOGIN_JSON" "$BASE_URL/api/auth/login") \
  || fail "cannot reach dashboard login at $BASE_URL"
[ "$CODE" = "200" ] || { cat "$BODY" >&2; fail "dashboard login returned HTTP $CODE"; }
AUTH=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/venue-private-auth") || fail "venue-private-auth endpoint unavailable"
printf '%s' "$AUTH" >"$BODY"
API_WALLET_ADDRESS=$(python3 - "$BODY" <<'PY'
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('status') != 'AUTHORIZATION_VERIFIED' or d.get('validated') is not True:
    print(json.dumps(d, indent=2, ensure_ascii=False), file=sys.stderr)
    raise SystemExit('STEP37: FAIL: Hyperliquid TESTNET account/API-wallet authorization is not verified')
if d.get('venue') != 'HYPERLIQUID' or d.get('targetEnvironment') != 'TESTNET':
    raise SystemExit('STEP37: FAIL: venue/environment identity mismatch')
if d.get('apiWalletRole') != 'agent' or d.get('apiWalletAuthorizedForAccount') is not True:
    raise SystemExit('STEP37: FAIL: configured API wallet is not an authorized agent for the configured account')
if d.get('remoteAuthorization') != 'VERIFIED':
    raise SystemExit('STEP37: FAIL: remote authorization was not verified')
if d.get('secretMountedInDashboard') is not False or d.get('orderRouting') != 'DISABLED' or d.get('readOnly') is not True:
    raise SystemExit('STEP37: FAIL: Step 37 crossed the dashboard read-only/secret-isolation boundary')
print(d.get('apiWalletAddress','').lower())
PY
) || exit 1
[ -n "$API_WALLET_ADDRESS" ] || fail "venue-private-auth returned an empty API-wallet address"
pass "Hyperliquid reports the configured TESTNET API wallet as agent for the configured account"

echo "[4/8] Local API-wallet secret-file safety"
[ -e "$KEY_FILE" ] || fail "API-wallet private-key file not found: $KEY_FILE"
[ -f "$KEY_FILE" ] || fail "API-wallet private-key path is not a regular file: $KEY_FILE"
[ ! -L "$KEY_FILE" ] || fail "API-wallet private-key file must not be a symlink"
python3 - "$KEY_FILE" <<'PY' || exit 1
import os, stat, sys
p=sys.argv[1]
mode=stat.S_IMODE(os.stat(p).st_mode)
if mode & 0o077:
    raise SystemExit(f'STEP37: FAIL: private-key file permissions are too broad ({mode:04o}); run chmod 600 "{p}"')
print(f'STEP37: PASS: local secret file permissions are {mode:04o}')
PY

echo "[5/8] Local key -> API-wallet address proof"
python3 - "$KEY_FILE" "$TMPDIR37/key.der" <<'PY' || exit 1
from pathlib import Path
import sys
raw=Path(sys.argv[1]).read_text(encoding='utf-8').strip()
if raw.startswith(('0x','0X')): raw=raw[2:]
if len(raw)!=64 or any(c not in '0123456789abcdefABCDEF' for c in raw):
    raise SystemExit('STEP37: FAIL: API-wallet private key must be exactly 32 bytes encoded as 64 hexadecimal characters (optional 0x prefix)')
d=int(raw,16)
n=0xFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFEBAAEDCE6AF48A03BBFD25E8CD0364141
if d <= 0 or d >= n:
    raise SystemExit('STEP37: FAIL: API-wallet private key scalar is outside secp256k1 range')
key=bytes.fromhex(raw)
body=b'\x02\x01\x01'+b'\x04\x20'+key+b'\xa0\x07\x06\x05\x2b\x81\x04\x00\x0a'
der=b'\x30'+bytes([len(body)])+body
Path(sys.argv[2]).write_bytes(der)
PY
command -v openssl >/dev/null 2>&1 || fail "openssl is required for the local secp256k1 ownership proof"
openssl ec -inform DER -in "$TMPDIR37/key.der" -pubout -conv_form uncompressed -outform DER -out "$TMPDIR37/pub.der" >/dev/null 2>&1 \
  || fail "openssl could not derive the secp256k1 public key"
python3 - "$TMPDIR37/pub.der" "$TMPDIR37/pub64.bin" <<'PY' || exit 1
from pathlib import Path
import sys
b=Path(sys.argv[1]).read_bytes()
if len(b)<65 or b[-65] != 4:
    raise SystemExit('STEP37: FAIL: unexpected OpenSSL secp256k1 public-key encoding')
Path(sys.argv[2]).write_bytes(b[-64:])
PY
if ! openssl list -digest-algorithms 2>/dev/null | grep -q 'KECCAK-256'; then
  fail "OpenSSL KECCAK-256 digest is required to derive the EVM agent address"
fi
openssl dgst -KECCAK-256 -binary "$TMPDIR37/pub64.bin" > "$TMPDIR37/keccak.bin" \
  || fail "OpenSSL KECCAK-256 address derivation failed"
DERIVED_ADDRESS=$(python3 - "$TMPDIR37/keccak.bin" <<'PY'
from pathlib import Path
import sys
h=Path(sys.argv[1]).read_bytes()
if len(h)!=32: raise SystemExit('STEP37: FAIL: unexpected KECCAK-256 output length')
print('0x'+h[-20:].hex())
PY
) || exit 1
[ "$DERIVED_ADDRESS" = "$API_WALLET_ADDRESS" ] || fail "local private key derives to $DERIVED_ADDRESS, not configured/approved API wallet $API_WALLET_ADDRESS"
pass "local private key derives exactly to the approved TESTNET API-wallet address"

echo "[6/8] Local secp256k1 signing self-test"
printf '%s\n' 'algoTrading Step 37 local signer self-test — no Hyperliquid action' > "$TMPDIR37/challenge.txt"
openssl ec -inform DER -in "$TMPDIR37/key.der" -out "$TMPDIR37/key.pem" >/dev/null 2>&1 || fail "could not prepare local signer key"
openssl ec -in "$TMPDIR37/key.pem" -pubout -out "$TMPDIR37/pub.pem" >/dev/null 2>&1 || fail "could not prepare local signer public key"
openssl dgst -sha256 -sign "$TMPDIR37/key.pem" -out "$TMPDIR37/signature.bin" "$TMPDIR37/challenge.txt" >/dev/null 2>&1 || fail "local secp256k1 signing failed"
openssl dgst -sha256 -verify "$TMPDIR37/pub.pem" -signature "$TMPDIR37/signature.bin" "$TMPDIR37/challenge.txt" >/dev/null 2>&1 || fail "local secp256k1 signature verification failed"
pass "local API-wallet key can sign; no Hyperliquid protocol payload was transmitted"

echo "[7/8] No trading command surface / manual route"
ROUTES=$(sed -n '/func (s \*Server) routes()/,/return s.middleware/p' "$ROOT/dashboard-api/internal/server/server.go")
printf '%s\n' "$ROUTES" | grep -Eq '/api/(submit|cancel|pause|resume|kill|order)' && fail "dangerous trading route found"
MANUAL=$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/manual-control") || fail "manual-control read model unavailable"
printf '%s' "$MANUAL" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('routeEnabled') is True:
    raise SystemExit('STEP37: FAIL: manual-control routeEnabled=true')
print('STEP37: PASS: manual routing remains fail-closed')
PY

echo "[8/8] Protocol-signing boundary remains explicit"
printf '%s' "$AUTH" >"$BODY"
python3 - "$BODY" <<'PY' || exit 1
import json, sys
d=json.load(open(sys.argv[1], encoding='utf-8'))
if d.get('protocolSigning') != 'NOT_EXERCISED':
    raise SystemExit('STEP37: FAIL: protocol signing must remain NOT_EXERCISED in Step 37')
if d.get('localKeyPossession') != 'LOCAL_GATE_REQUIRED':
    raise SystemExit('STEP37: FAIL: dashboard must not claim possession of the private key')
print('STEP37: PASS: Hyperliquid L1/EIP-712 protocol signing is still intentionally deferred')
PY

echo
echo "============================================================"
echo "STEP 37: PASS — TESTNET API WALLET CREDENTIAL + AUTHORIZATION VERIFIED"
echo "============================================================"
echo "Remote agent authorization matches the configured TESTNET account and the"
echo "local secret derives to that exact agent address. The private key never"
echo "entered dashboard-api. Hyperliquid /exchange signing and order routing are"
echo "still DISABLED and NOT EXERCISED."
echo "Next: Step 38 may read TESTNET account/positions/open-orders using the"
echo "configured ACCOUNT address. No real capital is required."
