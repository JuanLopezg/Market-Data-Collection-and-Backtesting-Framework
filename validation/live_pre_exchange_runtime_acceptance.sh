#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
TARGET="${2:-$(date -u -d 'yesterday' +%Y%m%d)}"
COMPOSE="$ROOT/deploy/live/docker-compose.yml"
ENVFILE="$ROOT/deploy/live/.env"

if [[ ! "$TARGET" =~ ^[0-9]{8}$ ]]; then
  echo "FAIL: target date must be YYYYMMDD; got '$TARGET'" >&2
  exit 2
fi
if [[ ! -f "$COMPOSE" ]]; then
  echo "FAIL: missing $COMPOSE" >&2
  exit 2
fi
if [[ ! -f "$ENVFILE" ]]; then
  echo "FAIL: missing $ENVFILE (copy .env.example and configure it first)" >&2
  exit 2
fi
if ! command -v docker >/dev/null 2>&1; then
  echo "FAIL: docker is required for runtime acceptance" >&2
  exit 2
fi

DC=(docker compose --env-file "$ENVFILE" -f "$COMPOSE")
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

for svc in market-data strategy portfolio-risk execution-state order-planner; do
  "${DC[@]}" logs --no-color "$svc" > "$TMP/$svc.log" 2>&1 || {
    echo "FAIL: cannot read docker logs for $svc" >&2
    exit 3
  }
done

fail=0

# Hard-failure markers must not be present in this run's service logs. This does
# not replace detailed log review; it catches obvious invalid acceptance runs.
for svc in market-data strategy portfolio-risk execution-state order-planner; do
  if grep -Eq 'event=fatal|checkpoint_conflict' "$TMP/$svc.log"; then
    echo "FAIL: fatal/checkpoint conflict found in $svc logs" >&2
    fail=1
  fi
done

# Durable daily checkpoints: the same T must exist in all three daily economic stores.
query_count() {
  local sql="$1"
  "${DC[@]}" exec -T postgres sh -lc \
    'psql -U "$POSTGRES_USER" -d "$POSTGRES_DB" -Atqc "$1"' \
    sh "$sql" 2>/dev/null | tr -d '[:space:]'
}

# Durable causal evidence for STEP 6F. Positive acceptance does not depend on
# Docker stdout: market-data may run as an ephemeral --run-once container and
# service stdout can be buffered. Persisted payloads are the source of truth.
expect_one() {
  local label="$1" sql="$2" count
  count="$(query_count "$sql")" || count="ERR"
  if [[ "$count" != "1" ]]; then
    echo "FAIL: $label; matching durable rows='$count'" >&2
    fail=1
  else
    echo "PASS: $label"
  fi
}

expect_one "MarketData update for $TARGET was durably consumed" \
  "SELECT count(*) FROM strategy_market_update_checkpoint WHERE state_key='strategy-service-market-db-v1' AND timestamp=$TARGET AND (update_payload::jsonb->>'completed_through')::bigint=$TARGET AND update_payload::jsonb->'metadata'->>'message_id'='market-data-updated:$TARGET';"

expect_one "StrategyIntent for exactly $TARGET is durable" \
  "SELECT count(*) FROM strategy_market_update_checkpoint WHERE state_key='strategy-service-market-db-v1' AND timestamp=$TARGET AND (intent_payload::jsonb->>'timestamp')::bigint=$TARGET AND intent_payload::jsonb->'metadata'->>'message_id'='strategy-intents:$TARGET';"

expect_one "ExecutionState daily AccountSnapshot for $TARGET reached PortfolioRisk" \
  "SELECT count(*) FROM portfolio_risk_live_account_checkpoint WHERE state_key='portfolio-risk-live-sqlite-v1' AND timestamp=$TARGET AND message_id='account-snapshot:market-data:$TARGET';"

expect_one "PortfolioRisk DecisionBatch for exactly $TARGET is durable" \
  "SELECT count(*) FROM portfolio_risk_live_decision_checkpoint WHERE state_key='portfolio-risk-live-sqlite-v1' AND timestamp=$TARGET AND (decision_payload::jsonb->>'decision_timestamp')::bigint=$TARGET AND decision_payload::jsonb->'metadata'->>'message_id'='portfolio-decision:$TARGET';"

expect_one "ExecutionState notional planning request for $TARGET reached OrderPlanner" \
  "SELECT count(*) FROM order_planner_live_notional_checkpoint WHERE state_key='order-planner-live-notional-v1' AND timestamp=$TARGET AND (request_payload::jsonb->>'decision_timestamp')::bigint=$TARGET AND request_payload::jsonb->'metadata'->>'message_id' LIKE 'notional-order-plan-request:$TARGET:%' AND (request_payload::jsonb->'reference_closes'->>'date')::bigint=$TARGET;"

expect_one "OrderPlanner NotionalOrderPlan for exactly $TARGET is durable" \
  "SELECT count(*) FROM order_planner_live_notional_checkpoint WHERE state_key='order-planner-live-notional-v1' AND timestamp=$TARGET AND (plan_payload::jsonb->>'decision_timestamp')::bigint=$TARGET AND plan_payload::jsonb->'metadata'->>'message_id' LIKE 'notional-order-plan:$TARGET:%' AND (plan_payload::jsonb->'reference_closes'->>'date')::bigint=$TARGET;"

expect_one "reference_close map is preserved unchanged through OrderPlanner" \
  "SELECT count(*) FROM order_planner_live_notional_checkpoint WHERE state_key='order-planner-live-notional-v1' AND timestamp=$TARGET AND request_payload::jsonb->'reference_closes'->'closes'=plan_payload::jsonb->'reference_closes'->'closes';"


strategy_count="$(query_count "SELECT count(*) FROM strategy_market_update_checkpoint WHERE state_key='strategy-service-market-db-v1' AND timestamp=$TARGET;")" || strategy_count="ERR"
risk_count="$(query_count "SELECT count(*) FROM portfolio_risk_live_decision_checkpoint WHERE state_key='portfolio-risk-live-sqlite-v1' AND timestamp=$TARGET;")" || risk_count="ERR"
planner_count="$(query_count "SELECT count(*) FROM order_planner_live_notional_checkpoint WHERE state_key='order-planner-live-notional-v1' AND timestamp=$TARGET;")" || planner_count="ERR"

for item in "strategy:$strategy_count" "portfolio-risk:$risk_count" "order-planner:$planner_count"; do
  svc="${item%%:*}"; count="${item#*:}"
  if [[ "$count" != "1" ]]; then
    echo "FAIL: expected exactly one durable $svc checkpoint for $TARGET; got '$count'" >&2
    fail=1
  else
    echo "PASS: exactly one durable $svc checkpoint exists for $TARGET"
  fi
done

if [[ $fail -ne 0 ]]; then
  echo "FAIL: STEP 6F runtime acceptance did not pass for $TARGET" >&2
  exit 1
fi

echo "PASS: STEP 6F pre-exchange runtime chain accepted for $TARGET"
echo 'PASS boundary: NotionalOrderPlan only. No real exchange order submission is part of STEP 6F.'
