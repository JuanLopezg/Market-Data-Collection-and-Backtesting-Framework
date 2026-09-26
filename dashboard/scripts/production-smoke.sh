#!/bin/sh
set -eu

ENV_FILE=${1:-.env.production}

fail() {
  echo "production-smoke: FAIL: $*" >&2
  exit 1
}

value_of() {
  sed -n "s/^$1=//p" "$ENV_FILE" | tail -n 1
}

json_escape() {
  # Production passwords should be single-line. Escape the JSON-sensitive chars.
  printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

[ -f "$ENV_FILE" ] || fail "$ENV_FILE not found"
DOMAIN=$(value_of DASHBOARD_DOMAIN)
USER=$(value_of DASHBOARD_VIEWER_USERNAME)
PASS=$(value_of DASHBOARD_VIEWER_PASSWORD)
[ -n "$USER" ] || USER=viewer
[ -n "$DOMAIN" ] || fail "DASHBOARD_DOMAIN is empty"
[ -n "$PASS" ] || fail "viewer password is empty"

BASE="https://$DOMAIN"
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step30-cookies.$$"
BODY="${TMPDIR:-/tmp}/control-dashboard-step30-body.$$"
trap 'rm -f "$COOKIE_JAR" "$BODY"' EXIT

echo "[1/7] HTTPS health"
curl -fsS --connect-timeout 5 --max-time 10 "$BASE/health" | grep -qx 'ok'

echo "[2/7] API liveness"
curl -fsS --connect-timeout 5 --max-time 10 "$BASE/api/health" >/dev/null

echo "[3/7] unauthenticated data is protected"
CODE=$(curl -sS -o "$BODY" -w '%{http_code}' --connect-timeout 5 --max-time 10 "$BASE/api/overview")
[ "$CODE" = "401" ] || fail "expected /api/overview HTTP 401 before login, got $CODE"

echo "[4/7] viewer login"
USER_JSON=$(json_escape "$USER")
PASS_JSON=$(json_escape "$PASS")
curl -fsS -c "$COOKIE_JAR" \
  -H 'Content-Type: application/json' \
  --data "{\"username\":\"$USER_JSON\",\"password\":\"$PASS_JSON\"}" \
  "$BASE/api/auth/login" >/dev/null

echo "[5/7] authenticated source status"
SOURCE=$(curl -fsS -b "$COOKIE_JAR" "$BASE/api/source-status")
printf '%s\n' "$SOURCE" | grep -q '"mode":"real"' || fail "source-status is not real mode"
printf '%s\n' "$SOURCE" | grep -q '"reachable":true' || fail "no reachable source reported"

echo "[6/7] diagnostics"
DIAG=$(curl -fsS -b "$COOKIE_JAR" "$BASE/api/diagnostics")
printf '%s\n' "$DIAG" | grep -q '"http"' || fail "diagnostics missing http section"
printf '%s\n' "$DIAG" | grep -q '"sse"' || fail "diagnostics missing sse section"

echo "[7/7] SSE handshake through Caddy"
# The stream sends a connected event immediately, so a short timeout is enough.
set +e
curl -sS -N --max-time 3 -b "$COOKIE_JAR" "$BASE/api/stream" >"$BODY"
CURL_RC=$?
set -e
# curl normally exits 28 because we intentionally stop a long-lived stream.
[ "$CURL_RC" -eq 0 ] || [ "$CURL_RC" -eq 28 ] || fail "SSE curl failed with code $CURL_RC"
grep -q 'event: connected' "$BODY" || fail "SSE connected event not observed"

echo "STEP 30 PRODUCTION SMOKE: PASS"
