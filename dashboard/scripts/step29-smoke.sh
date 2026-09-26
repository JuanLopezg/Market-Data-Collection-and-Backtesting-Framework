#!/bin/sh
set -eu

BASE_URL="${DASHBOARD_BASE_URL:-http://localhost:8080}"
USER="${DASHBOARD_SMOKE_USER:-viewer}"
PASS="${DASHBOARD_SMOKE_PASSWORD:-viewer-demo}"
COOKIE_JAR="${TMPDIR:-/tmp}/control-dashboard-step29-cookies.$$"
trap 'rm -f "$COOKIE_JAR"' EXIT

echo "[1/4] health"
curl -fsS "$BASE_URL/api/health" >/dev/null

echo "[2/4] bounded health burst (40 requests / 8 workers)"
seq 1 40 | xargs -P 8 -I{} sh -c 'curl -fsS "$0/api/health" >/dev/null' "$BASE_URL"

echo "[3/4] login"
curl -fsS -c "$COOKIE_JAR" \
  -H 'Content-Type: application/json' \
  -d "{\"username\":\"$USER\",\"password\":\"$PASS\"}" \
  "$BASE_URL/api/auth/login" >/dev/null

echo "[4/4] diagnostics"
DIAG="$(curl -fsS -b "$COOKIE_JAR" "$BASE_URL/api/diagnostics")"
printf '%s\n' "$DIAG" | grep -q '"http"'
printf '%s\n' "$DIAG" | grep -q '"sse"'
printf '%s\n' "$DIAG" | grep -q '"runtime"'
printf '%s\n' "$DIAG"

echo "STEP 29 SMOKE: PASS"
