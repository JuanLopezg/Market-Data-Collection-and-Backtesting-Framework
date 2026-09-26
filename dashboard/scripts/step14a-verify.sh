#!/usr/bin/env sh
set -eu

ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
API="$ROOT/dashboard-api"

printf '%s\n' '============================================================'
printf '%s\n' 'CONTROL DASHBOARD STEP 14A VERIFY'
printf '%s\n' '============================================================'

cd "$API"

unformatted="$(gofmt -l ./cmd ./internal)"
if [ -n "$unformatted" ]; then
  printf '%s\n' 'FAIL: gofmt required:' >&2
  printf '%s\n' "$unformatted" >&2
  exit 1
fi
printf '%s\n' 'PASS: gofmt'

go test ./...
printf '%s\n' 'PASS: go test ./...'

go vet ./...
printf '%s\n' 'PASS: go vet ./...'

go build ./...
printf '%s\n' 'PASS: go build ./...'

if grep -Rni --include='subjects.go' 'clock\.' "$API/internal/integration/nats" >/dev/null 2>&1; then
  printf '%s\n' 'FAIL: legacy clock subject found in verified dashboard mapping' >&2
  exit 1
fi
printf '%s\n' 'PASS: no legacy clock subjects in verified NATS mapping'

if ! grep -q 'trading_runtime_state' "$API/internal/integration/postgres/schema.go"; then
  printf '%s\n' 'FAIL: trading_runtime_state mapping missing' >&2
  exit 1
fi
if ! grep -q 'portfolio_risk_live_decision_checkpoint' "$API/internal/integration/postgres/schema.go"; then
  printf '%s\n' 'FAIL: PortfolioRisk checkpoint mapping missing' >&2
  exit 1
fi
if ! grep -q 'order_planner_live_notional_checkpoint' "$API/internal/integration/postgres/schema.go"; then
  printf '%s\n' 'FAIL: OrderPlanner checkpoint mapping missing' >&2
  exit 1
fi
printf '%s\n' 'PASS: verified PostgreSQL source map present'

printf '%s\n' '============================================================'
printf '%s\n' 'STEP 14A: PASS'
printf '%s\n' 'RealProvider remains fail-closed by design; the next integration step enables concrete reads.'
printf '%s\n' '============================================================'
