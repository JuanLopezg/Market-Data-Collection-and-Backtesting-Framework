#!/bin/sh
set -eu

ENV_FILE=${1:-.env.production}
BASE_COMPOSE=${2:-docker-compose.production.yml}
REAL_COMPOSE=${3:-docker-compose.production.real.yml}

fail() {
  echo "preflight: FAIL: $*" >&2
  exit 1
}

value_of() {
  sed -n "s/^$1=//p" "$ENV_FILE" | tail -n 1
}

[ -f "$ENV_FILE" ] || fail "$ENV_FILE not found (copy .env.production.example first)"
[ -f "$BASE_COMPOSE" ] || fail "$BASE_COMPOSE not found"
[ -f "$REAL_COMPOSE" ] || fail "$REAL_COMPOSE not found"

DOMAIN=$(value_of DASHBOARD_DOMAIN)
VIEWER_PASSWORD=$(value_of DASHBOARD_VIEWER_PASSWORD)
OPERATOR_PASSWORD=$(value_of DASHBOARD_OPERATOR_PASSWORD)
DATA_PROVIDER=$(value_of DASHBOARD_DATA_PROVIDER)
POSTGRES_DSN=$(value_of DASHBOARD_POSTGRES_DSN)
NATS_URL=$(value_of DASHBOARD_NATS_URL)
MARKET_DIR=$(value_of DASHBOARD_MARKET_DATA_HOST_DIR)
MARKET_DB=$(value_of DASHBOARD_MARKET_DATA_DB_HOST_FILE)
LIVE_NETWORK=$(value_of ALGOTRADING_LIVE_NETWORK)

[ -n "$DOMAIN" ] || fail "DASHBOARD_DOMAIN is empty"
[ "$DOMAIN" != "dashboard.example.com" ] || fail "replace dashboard.example.com with the real hostname"
case "$DOMAIN" in
  http://*|https://*|*/*|*:* ) fail "DASHBOARD_DOMAIN must be a hostname only" ;;
esac

[ -n "$VIEWER_PASSWORD" ] || fail "DASHBOARD_VIEWER_PASSWORD is empty"
[ -n "$OPERATOR_PASSWORD" ] || fail "DASHBOARD_OPERATOR_PASSWORD is empty"
[ "$VIEWER_PASSWORD" != "REPLACE_WITH_A_LONG_RANDOM_SECRET" ] || fail "replace the viewer example password"
[ "$OPERATOR_PASSWORD" != "REPLACE_WITH_A_DIFFERENT_LONG_RANDOM_SECRET" ] || fail "replace the operator example password"
[ "$VIEWER_PASSWORD" != "$OPERATOR_PASSWORD" ] || fail "viewer and operator passwords must be different"
[ ${#VIEWER_PASSWORD} -ge 20 ] || fail "viewer password must be at least 20 characters"
[ ${#OPERATOR_PASSWORD} -ge 20 ] || fail "operator password must be at least 20 characters"

[ "$DATA_PROVIDER" = "real" ] || fail "production Step 30 requires DASHBOARD_DATA_PROVIDER=real"
[ -n "$POSTGRES_DSN" ] || fail "DASHBOARD_POSTGRES_DSN is empty"
case "$POSTGRES_DSN" in
  *REPLACE_WITH_POSTGRES_PASSWORD*) fail "replace PostgreSQL example password" ;;
esac
[ -n "$NATS_URL" ] || fail "DASHBOARD_NATS_URL is empty"
[ -n "$MARKET_DIR" ] || fail "DASHBOARD_MARKET_DATA_HOST_DIR is empty"
[ -n "$MARKET_DB" ] || fail "DASHBOARD_MARKET_DATA_DB_HOST_FILE is empty"
case "$MARKET_DIR" in /*) ;; *) fail "DASHBOARD_MARKET_DATA_HOST_DIR must be an absolute path" ;; esac
case "$MARKET_DB" in /*) ;; *) fail "DASHBOARD_MARKET_DATA_DB_HOST_FILE must be an absolute path" ;; esac
[ -d "$MARKET_DIR" ] || fail "market-data directory does not exist: $MARKET_DIR"
[ -f "$MARKET_DB" ] || fail "market-data database does not exist: $MARKET_DB"
[ -r "$MARKET_DB" ] || fail "market-data database is not readable: $MARKET_DB"

# Static boundary checks that do not require a running stack.
grep -q 'internal: true' "$BASE_COMPOSE" || fail "dashboard-private network is not internal"
if awk '/^  dashboard-api:/{in_api=1;next} /^  dashboard-web:/{in_api=0} in_api && /^    ports:/{found=1} END{exit found?0:1}' "$BASE_COMPOSE"; then
  fail "dashboard-api unexpectedly publishes host ports"
fi
grep -q 'DASHBOARD_AUTH_ALLOW_DEMO: "false"' "$BASE_COMPOSE" || fail "demo authentication is not disabled"
grep -q 'DASHBOARD_AUTH_COOKIE_SECURE: "true"' "$BASE_COMPOSE" || fail "Secure auth cookie is not forced"
grep -q 'cap_drop:' "$BASE_COMPOSE" || fail "capability drop missing"
grep -q 'no-new-privileges:true' "$BASE_COMPOSE" || fail "no-new-privileges missing"
grep -q 'read_only: true' "$BASE_COMPOSE" || fail "read-only root filesystem missing"
grep -q '"80:80"' "$BASE_COMPOSE" || fail "TCP 80 is not published by dashboard-web"
grep -q '"443:443"' "$BASE_COMPOSE" || fail "TCP 443 is not published by dashboard-web"

command -v docker >/dev/null 2>&1 || fail "docker is not installed"
docker compose version >/dev/null 2>&1 || fail "docker compose plugin is unavailable"

docker compose --env-file "$ENV_FILE" -f "$BASE_COMPOSE" -f "$REAL_COMPOSE" config >/dev/null
echo "preflight: compose config: PASS"

if [ -z "$LIVE_NETWORK" ]; then
  LIVE_NETWORK=algotrading-live_live
fi
docker network inspect "$LIVE_NETWORK" >/dev/null 2>&1 || fail "trading Docker network not found: $LIVE_NETWORK (start deploy/live first)"
echo "preflight: trading network: PASS ($LIVE_NETWORK)"

# Best-effort DNS warning. Caddy still performs the authoritative ACME check.
if command -v getent >/dev/null 2>&1; then
  if getent ahosts "$DOMAIN" >/dev/null 2>&1; then
    echo "preflight: DNS resolves: PASS ($DOMAIN)"
  else
    echo "preflight: WARN: $DOMAIN does not resolve yet; TLS issuance will fail until DNS is correct" >&2
  fi
fi

echo "preflight: public boundary: PASS (web only on 80/443; API private)"
echo "preflight: auth hardening: PASS"
echo "preflight: real-source configuration: PASS"
echo "preflight: market DB: $MARKET_DB"
echo "preflight: READY TO BUILD (this does not prove exchange/trading readiness)"
