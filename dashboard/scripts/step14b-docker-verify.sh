#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
TMP_DOCKER_CONFIG="$(mktemp -d)"
trap 'rm -rf "$TMP_DOCKER_CONFIG"' EXIT INT TERM
printf '%s\n' '{"auths":{}}' > "$TMP_DOCKER_CONFIG/config.json"

printf '%s\n' '============================================================'
printf '%s\n' 'CONTROL DASHBOARD STEP 14B.1 DOCKER VERIFY'
printf '%s\n' '============================================================'

DOCKER_CONFIG="$TMP_DOCKER_CONFIG" docker build \
  --target verify \
  -t control-dashboard-step14b-api-verify \
  "$ROOT/dashboard-api"

DOCKER_CONFIG="$TMP_DOCKER_CONFIG" docker build \
  --target build \
  -t control-dashboard-step14b-web-build \
  "$ROOT"

printf '%s\n' 'PASS: dashboard-api Go/libpq build + tests'
printf '%s\n' 'PASS: React/TypeScript/Vite production build'
printf '%s\n' '============================================================'
printf '%s\n' 'STEP 14B.1: PASS'
printf '%s\n' '14B is NOT closed until the PostgreSQL smoke gate 14B.2 also passes.'
printf '%s\n' '============================================================'
