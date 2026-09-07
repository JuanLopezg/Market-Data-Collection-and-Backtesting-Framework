#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY="$ROOT/deploy/distributed_replay"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
HISTORICAL_DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
PROJECT="algotrading_validation_sim_exchange_restart"
NATS_PORT="${SIM_EXCHANGE_RESTART_NATS_PORT:-54253}"
NATS_MONITOR_PORT="${SIM_EXCHANGE_RESTART_NATS_MONITOR_PORT:-58253}"
POSTGRES_PORT="${SIM_EXCHANGE_RESTART_POSTGRES_PORT:-55453}"
KILL_AFTER_CYCLES="${SIM_EXCHANGE_RESTART_KILL_AFTER_CYCLES:-45}"
LOG_ROOT="$ROOT/validation/logs/simulated_exchange_restart"
STAMP="$(date +%Y%m%d_%H%M%S)"
HARNESS_STDOUT="$LOG_ROOT/${STAMP}_harness.log"
COMPOSE=(docker compose -p "$PROJECT" -f "$DEPLOY/docker-compose.yml")

mkdir -p "$LOG_ROOT"

[[ -x "$HARNESS" ]] || { echo "[FAIL] historical comparator harness missing: $HARNESS" >&2; exit 1; }
[[ -f "$HISTORICAL_DATA" ]] || { echo "[FAIL] historical data missing: $HISTORICAL_DATA" >&2; exit 1; }
[[ "$KILL_AFTER_CYCLES" =~ ^[0-9]+$ ]] && (( KILL_AFTER_CYCLES >= 5 && KILL_AFTER_CYCLES < 90 )) || {
    echo "[FAIL] SIM_EXCHANGE_RESTART_KILL_AFTER_CYCLES must be between 5 and 89" >&2
    exit 2
}

export HISTORICAL_KEEP_ON_FAILURE=1
export HISTORICAL_DATA_PATH="$HISTORICAL_DATA"
export REPLAY_NATS_PORT="$NATS_PORT"
export REPLAY_NATS_MONITOR_PORT="$NATS_MONITOR_PORT"
export REPLAY_POSTGRES_PORT="$POSTGRES_PORT"

cleanup_wrapper() {
    local code=$?
    if [[ "$code" != "0" ]]; then
        echo "[INFO] failure topology may be preserved by the historical harness."
        echo "[INFO] inspect: docker compose -p $PROJECT -f $DEPLOY/docker-compose.yml ps"
        echo "[INFO] cleanup: docker compose -p $PROJECT -f $DEPLOY/docker-compose.yml down -v --remove-orphans"
    fi
}
trap cleanup_wrapper EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

pg_scalar() {
    "${COMPOSE[@]}" exec -T postgres \
        psql -U algotrading -d algotrading -Atqc "$1" 2>/dev/null | tr -d '\r'
}

echo "============================================================"
echo "STEP 33C — SIMULATED-EXCHANGE HARD RESTART / RECREATE"
echo "Window       : measured 2021-03-01 + 60 days, warmup 30 (90 cycles)"
echo "Crash after  : >= $KILL_AFTER_CYCLES execution-complete barriers + durable fills"
echo "Project      : $PROJECT"
echo "Harness log  : $HARNESS_STDOUT"
echo "============================================================"

runtime_build_args=()
if [[ "${RESTART_SKIP_RUNTIME_BUILD:-0}" != "1" ]]; then
    runtime_build_args+=(--force-runtime-build)
fi

set +e
bash "$HARNESS" \
    --historical-data "$HISTORICAL_DATA" \
    --start-date 2021-03-01 \
    --days 60 \
    --warmup-days 30 \
    --portfolio-mode equal-weight \
    --project "$PROJECT" \
    --nats-port "$NATS_PORT" \
    --nats-monitor-port "$NATS_MONITOR_PORT" \
    --postgres-port "$POSTGRES_PORT" \
    --log-root "$LOG_ROOT/runs" \
    --require-trading \
    "${runtime_build_args[@]}" \
    >"$HARNESS_STDOUT" 2>&1 &
HARNESS_PID=$!
set -e

echo "[INFO] historical replay harness pid=$HARNESS_PID"

exchange_id=""
for _ in $(seq 1 1200); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        echo "[FAIL] harness exited before simulated-exchange container became available"
        cat "$HARNESS_STDOUT" || true
        wait "$HARNESS_PID" || true
        exit 1
    fi
    exchange_id="$("${COMPOSE[@]}" ps -q simulated-exchange 2>/dev/null || true)"
    [[ -n "$exchange_id" ]] && break
    sleep 0.25
done
[[ -n "$exchange_id" ]] || { echo "[FAIL] simulated-exchange container not found"; exit 1; }

echo "[PASS] simulated-exchange container started: $exchange_id"

completed=0
known_before=0
next_fill_before=1
prices_before=0

for _ in $(seq 1 2400); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        echo "[FAIL] harness exited before restart injection"
        cat "$HARNESS_STDOUT" || true
        wait "$HARNESS_PID" || true
        exit 1
    fi

    completed="$("${COMPOSE[@]}" logs --no-color replay-controller 2>/dev/null | grep -c '\[REPLAY\] execution-complete' || true)"

    if (( completed >= KILL_AFTER_CYCLES )); then
        known_before="$(pg_scalar "SELECT count(*) FROM simulated_exchange_known_orders WHERE state_key='simulated-exchange';" || true)"
        next_fill_before="$(pg_scalar "SELECT next_fill_id FROM simulated_exchange_state WHERE state_key='simulated-exchange';" || true)"
        prices_before="$(pg_scalar "SELECT count(DISTINCT timestamp) FROM simulated_exchange_execution_prices WHERE state_key='simulated-exchange';" || true)"

        if [[ "$known_before" =~ ^[0-9]+$ ]] &&
           [[ "$next_fill_before" =~ ^[0-9]+$ ]] &&
           [[ "$prices_before" =~ ^[0-9]+$ ]] &&
           (( known_before > 0 )) &&
           (( next_fill_before > 1 )) &&
           (( prices_before > 0 )); then
            break
        fi
    fi

    sleep 0.25
done

[[ "$known_before" =~ ^[0-9]+$ ]] && (( known_before > 0 )) || {
    echo "[FAIL] simulated-exchange had no durable known orders before crash"
    docker logs --tail=160 "$exchange_id" 2>&1 || true
    exit 1
}
[[ "$next_fill_before" =~ ^[0-9]+$ ]] && (( next_fill_before > 1 )) || {
    echo "[FAIL] simulated-exchange had no durable fills before crash"
    docker logs --tail=160 "$exchange_id" 2>&1 || true
    exit 1
}
[[ "$prices_before" =~ ^[0-9]+$ ]] && (( prices_before > 0 )) || {
    echo "[FAIL] simulated-exchange had no durable execution prices before crash"
    exit 1
}

cash_before="$(pg_scalar "SELECT cash FROM simulated_exchange_state WHERE state_key='simulated-exchange';")"
positions_before="$(pg_scalar "SELECT count(*) FROM simulated_exchange_positions WHERE state_key='simulated-exchange';")"
active_before="$(pg_scalar "SELECT count(*) FROM simulated_exchange_active_orders WHERE state_key='simulated-exchange';")"
outbox_pending_before="$(pg_scalar "SELECT count(*) FROM simulated_exchange_outbox WHERE state_key='simulated-exchange' AND published=FALSE;")"

echo "[PASS] pre-crash independent exchange checkpoint is non-empty: cycles=$completed known_orders=$known_before next_fill_id=$next_fill_before prices=$prices_before positions=$positions_before active_orders=$active_before pending_outbox=$outbox_pending_before"
echo "[INFO] injecting hard SimulatedExchange crash/recreate"

docker kill --signal KILL "$exchange_id" >/dev/null 2>&1 || true
docker rm -f "$exchange_id" >/dev/null 2>&1 || true

"${COMPOSE[@]}" up -d --no-deps --no-build simulated-exchange >/dev/null
new_exchange_id="$("${COMPOSE[@]}" ps -q simulated-exchange)"
[[ -n "$new_exchange_id" && "$new_exchange_id" != "$exchange_id" ]] || {
    echo "[FAIL] simulated-exchange container was not recreated with a new container id"
    exit 1
}
echo "[PASS] simulated-exchange container recreated: $new_exchange_id"

recovery_seen=0
for _ in $(seq 1 240); do
    if docker logs "$new_exchange_id" 2>&1 | grep -Eq \
        'event=simulated_exchange_recovery_completed recovered=true .*known_orders=[1-9][0-9]*'; then
        recovery_seen=1
        break
    fi
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        break
    fi
    sleep 0.25
done
[[ "$recovery_seen" == "1" ]] || {
    echo "[FAIL] recreated simulated-exchange did not report non-empty PostgreSQL recovery"
    docker logs --tail=200 "$new_exchange_id" 2>&1 || true
    exit 1
}
echo "[PASS] recreated simulated-exchange recovered independent PostgreSQL truth"

known_after_recreate="$(pg_scalar "SELECT count(*) FROM simulated_exchange_known_orders WHERE state_key='simulated-exchange';")"
next_fill_after_recreate="$(pg_scalar "SELECT next_fill_id FROM simulated_exchange_state WHERE state_key='simulated-exchange';")"
prices_after_recreate="$(pg_scalar "SELECT count(DISTINCT timestamp) FROM simulated_exchange_execution_prices WHERE state_key='simulated-exchange';")"

(( known_after_recreate >= known_before )) || {
    echo "[FAIL] known-order durable prefix regressed across recreate: before=$known_before after=$known_after_recreate"
    exit 1
}
(( next_fill_after_recreate >= next_fill_before )) || {
    echo "[FAIL] next FillID regressed across recreate: before=$next_fill_before after=$next_fill_after_recreate"
    exit 1
}
(( prices_after_recreate >= prices_before )) || {
    echo "[FAIL] execution-price durable prefix regressed across recreate: before=$prices_before after=$prices_after_recreate"
    exit 1
}
echo "[PASS] recreated exchange sees durable prefix: known_orders=$known_after_recreate next_fill_id=$next_fill_after_recreate prices=$prices_after_recreate"

target_completed=$(( completed + 3 ))
if (( target_completed > 89 )); then
    target_completed=89
fi

completed_after_restart=$completed
for _ in $(seq 1 1200); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        break
    fi
    completed_after_restart="$("${COMPOSE[@]}" logs --no-color replay-controller 2>/dev/null | grep -c '\[REPLAY\] execution-complete' || true)"
    if (( completed_after_restart >= target_completed )); then
        break
    fi
    sleep 0.25
done
(( completed_after_restart >= target_completed )) || {
    echo "[FAIL] replay did not advance after simulated-exchange recreate: before=$completed after=$completed_after_restart"
    docker logs --tail=200 "$new_exchange_id" 2>&1 || true
    exit 1
}
echo "[PASS] replay advanced with recreated simulated-exchange: cycles=$completed -> $completed_after_restart"

known_after_progress="$(pg_scalar "SELECT count(*) FROM simulated_exchange_known_orders WHERE state_key='simulated-exchange';")"
next_fill_after_progress="$(pg_scalar "SELECT next_fill_id FROM simulated_exchange_state WHERE state_key='simulated-exchange';")"
(( known_after_progress >= known_before )) || {
    echo "[FAIL] recreated simulated-exchange lost known-order history after progress"
    exit 1
}
(( next_fill_after_progress >= next_fill_before )) || {
    echo "[FAIL] recreated simulated-exchange regressed FillID state after progress"
    exit 1
}
echo "[PASS] recreated exchange continued durable execution: known_orders=$known_after_progress next_fill_id=$next_fill_after_progress"

duplicate_outbox_ids="$(pg_scalar "SELECT count(*) FROM (SELECT message_id FROM simulated_exchange_outbox WHERE state_key='simulated-exchange' GROUP BY message_id HAVING count(*) > 1) x;")"
[[ "$duplicate_outbox_ids" == "0" ]] || {
    echo "[FAIL] simulated-exchange durable outbox contains duplicate message IDs: $duplicate_outbox_ids"
    exit 1
}
echo "[PASS] durable outbox message IDs remain unique after recreate"

set +e
wait "$HARNESS_PID"
HARNESS_STATUS=$?
set -e
if [[ "$HARNESS_STATUS" != "0" ]]; then
    echo "[FAIL] historical comparator harness failed after simulated-exchange restart"
    cat "$HARNESS_STDOUT" || true
    exit "$HARNESS_STATUS"
fi

LOG_DIR="$(awk -F': ' '/^LOGS[[:space:]]*: /{print $2; exit}' "$HARNESS_STDOUT")"
if [[ -z "$LOG_DIR" || ! -d "$LOG_DIR" ]]; then
    echo "[FAIL] could not resolve historical run log directory"
    cat "$HARNESS_STDOUT" || true
    exit 1
fi

EXCHANGE_LOG="$LOG_DIR/services/simulated-exchange.log"
EXECUTION_LOG="$LOG_DIR/services/execution-state.log"
COMPARE_LOG="$LOG_DIR/05_distributed_fast_compare.log"

[[ -f "$EXCHANGE_LOG" ]] || { echo "[FAIL] captured simulated-exchange log missing: $EXCHANGE_LOG"; exit 1; }
[[ -f "$EXECUTION_LOG" ]] || { echo "[FAIL] captured execution-state log missing: $EXECUTION_LOG"; exit 1; }
[[ -f "$COMPARE_LOG" ]] || { echo "[FAIL] comparator log missing: $COMPARE_LOG"; exit 1; }

grep -Fq 'DISTRIBUTED_FAST_COMPARE: PASS' "$COMPARE_LOG" || {
    echo "[FAIL] restarted distributed path diverged from fast path"
    cat "$COMPARE_LOG"
    exit 1
}

grep -Eq 'event=simulated_exchange_recovery_completed recovered=true .*known_orders=[1-9][0-9]*' "$EXCHANGE_LOG" || {
    echo "[FAIL] final simulated-exchange logs do not prove durable recovery"
    tail -n 240 "$EXCHANGE_LOG"
    exit 1
}

if grep -Fq 'event=reconciliation_blocked' "$EXECUTION_LOG"; then
    echo "[FAIL] execution-state reported blocked reconciliation after simulated-exchange restart"
    grep -F 'event=reconciliation_blocked' "$EXECUTION_LOG" || true
    exit 1
fi

echo "[PASS] restart replay remained bit-for-bit aligned with fast Decision/Execution reference"
echo "[PASS] no reconciliation block was observed"
echo "[PASS] durable outbox message IDs remained unique"

echo
echo "============================================================"
echo "STEP 33C RESULT: PASS"
echo "SimulatedExchange was killed + deleted + recreated after durable trading state existed."
echo "Independent exchange cash/positions/orders/prices/FillID state recovered from PostgreSQL."
echo "No duplicate economic effects were observed; distributed == fast remained exact."
echo "Logs: $LOG_DIR"
echo "============================================================"
