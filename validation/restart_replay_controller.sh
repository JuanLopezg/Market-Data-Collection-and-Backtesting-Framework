#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY="$ROOT/deploy/distributed_replay"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
HISTORICAL_DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
PROJECT="algotrading_validation_replay_restart"
NATS_PORT="${REPLAY_RESTART_NATS_PORT:-54253}"
NATS_MONITOR_PORT="${REPLAY_RESTART_NATS_MONITOR_PORT:-58253}"
POSTGRES_PORT="${REPLAY_RESTART_POSTGRES_PORT:-55453}"
KILL_AFTER_CYCLES="${REPLAY_RESTART_KILL_AFTER_CYCLES:-45}"
LOG_ROOT="$ROOT/validation/logs/replay_controller_restart"
STAMP="$(date +%Y%m%d_%H%M%S)"
HARNESS_STDOUT="$LOG_ROOT/${STAMP}_harness.log"
COMPOSE=(docker compose -p "$PROJECT" -f "$DEPLOY/docker-compose.yml")

mkdir -p "$LOG_ROOT"

[[ -x "$HARNESS" ]] || { echo "[FAIL] historical comparator harness missing: $HARNESS" >&2; exit 1; }
[[ -f "$HISTORICAL_DATA" ]] || { echo "[FAIL] historical data missing: $HISTORICAL_DATA" >&2; exit 1; }
[[ "$KILL_AFTER_CYCLES" =~ ^[0-9]+$ ]] && (( KILL_AFTER_CYCLES >= 5 && KILL_AFTER_CYCLES < 90 )) || {
    echo "[FAIL] REPLAY_RESTART_KILL_AFTER_CYCLES must be between 5 and 89" >&2
    exit 2
}

export HISTORICAL_KEEP_ON_FAILURE=1
export HISTORICAL_DATA_PATH="$HISTORICAL_DATA"
export REPLAY_NATS_PORT="$NATS_PORT"
export REPLAY_NATS_MONITOR_PORT="$NATS_MONITOR_PORT"
export REPLAY_POSTGRES_PORT="$POSTGRES_PORT"

planner_paused=0
cleanup_wrapper() {
    local code=$?
    if [[ "$planner_paused" == "1" ]]; then
        "${COMPOSE[@]}" unpause order-planner >/dev/null 2>&1 || true
    fi
    if [[ "$code" != "0" ]]; then
        echo "[INFO] failure topology may be preserved by the historical harness."
        echo "[INFO] inspect: docker compose -p $PROJECT -f $DEPLOY/docker-compose.yml ps"
        echo "[INFO] cleanup: docker compose -p $PROJECT -f $DEPLOY/docker-compose.yml down -v --remove-orphans"
    fi
}
trap cleanup_wrapper EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "============================================================"
echo "STEP 33B — REPLAY-CONTROLLER MID-PHASE HARD RESTART"
echo "Window       : measured 2021-03-01 + 60 days, warmup 30 (90 cycles)"
echo "Crash target : >= $KILL_AFTER_CYCLES completed cycles, then WAIT_EXECUTION phase"
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
    --allow-controller-recreate \
    "${runtime_build_args[@]}" \
    >"$HARNESS_STDOUT" 2>&1 &
HARNESS_PID=$!
set -e

echo "[INFO] historical replay harness pid=$HARNESS_PID"

controller_id=""
planner_id=""
postgres_id=""
for _ in $(seq 1 1200); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        echo "[FAIL] harness exited before restart-test containers became available"
        cat "$HARNESS_STDOUT" || true
        wait "$HARNESS_PID" || true
        exit 1
    fi
    controller_id="$("${COMPOSE[@]}" ps -q replay-controller 2>/dev/null || true)"
    planner_id="$("${COMPOSE[@]}" ps -q order-planner 2>/dev/null || true)"
    postgres_id="$("${COMPOSE[@]}" ps -q postgres 2>/dev/null || true)"
    [[ -n "$controller_id" && -n "$planner_id" && -n "$postgres_id" ]] && break
    sleep 0.25
done
[[ -n "$controller_id" && -n "$planner_id" && -n "$postgres_id" ]] || {
    echo "[FAIL] required containers not found"
    exit 1
}

echo "[PASS] replay-controller started: $controller_id"

completed=0
while (( completed < KILL_AFTER_CYCLES )); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        echo "[FAIL] harness exited before restart injection"
        cat "$HARNESS_STDOUT" || true
        wait "$HARNESS_PID" || true
        exit 1
    fi
    completed="$(docker logs "$controller_id" 2>&1 | grep -c '\[REPLAY\] execution-complete' || true)"
    sleep 0.25
done

echo "[INFO] reached completed_cycles=$completed; pausing order-planner to hold next cycle in WAIT_EXECUTION"
"${COMPOSE[@]}" pause order-planner >/dev/null
planner_paused=1

# One in-flight plan can finish while the pause command is racing the controller.
# Re-sample the completed count after the process is definitely frozen, then wait
# for an OPEN release strictly beyond that completed prefix.
sleep 0.5
completed="$(docker logs "$controller_id" 2>&1 | grep -c '\[REPLAY\] execution-complete' || true)"

phase_ready=0
for _ in $(seq 1 480); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        echo "[FAIL] harness exited before WAIT_EXECUTION phase was established"
        cat "$HARNESS_STDOUT" || true
        wait "$HARNESS_PID" || true
        exit 1
    fi

    open_count="$(docker logs "$controller_id" 2>&1 | grep -c 'event=market_release_published kind=execution_open' || true)"
    decision_count="$(docker logs "$controller_id" 2>&1 | grep -c '\[REPLAY\] decision-ready' || true)"
    complete_now="$(docker logs "$controller_id" 2>&1 | grep -c '\[REPLAY\] execution-complete' || true)"

    if (( open_count > complete_now && decision_count > complete_now )); then
        completed="$complete_now"
        phase_ready=1
        break
    fi
    sleep 0.25
done
[[ "$phase_ready" == "1" ]] || {
    echo "[FAIL] could not establish replay-controller WAIT_EXECUTION phase"
    docker logs --tail=160 "$controller_id" || true
    exit 1
}

db_decisions="$(docker exec "$postgres_id" psql -U algotrading -d algotrading -tAc \
    "SELECT count(*) FROM replay_controller_decision_checkpoint WHERE state_key='replay-controller';" | tr -d '[:space:]')"
db_executions="$(docker exec "$postgres_id" psql -U algotrading -d algotrading -tAc \
    "SELECT count(*) FROM replay_controller_execution_checkpoint WHERE state_key='replay-controller';" | tr -d '[:space:]')"

[[ "$db_decisions" =~ ^[0-9]+$ && "$db_executions" =~ ^[0-9]+$ ]] || {
    echo "[FAIL] could not read replay checkpoint counts from PostgreSQL"
    exit 1
}
(( db_decisions > db_executions )) || {
    echo "[FAIL] expected durable decision without execution before crash; decisions=$db_decisions executions=$db_executions"
    exit 1
}

echo "[PASS] mid-phase checkpoint confirmed: decisions=$db_decisions executions=$db_executions"

# The historical harness resolves and exports the actual replay range in its own
# child shell. This wrapper is a separate parent shell, so a direct `docker compose
# up replay-controller` here would otherwise fall back to docker-compose.yml's
# default dates (2024-01-01..2024-01-03). The controller correctly rejects that
# mismatch against its durable PostgreSQL checkpoint. Recover the canonical range
# from PostgreSQL and export it before recreating the container.
db_range_start="$(docker exec "$postgres_id" psql -U algotrading -d algotrading -tAc \
    "SELECT range_start::text FROM replay_controller_metadata WHERE state_key='replay-controller';" | tr -d '[:space:]')"
db_range_end="$(docker exec "$postgres_id" psql -U algotrading -d algotrading -tAc \
    "SELECT range_end::text FROM replay_controller_metadata WHERE state_key='replay-controller';" | tr -d '[:space:]')"

compact_timestamp_to_iso_date() {
    local value="$1"
    [[ "$value" =~ ^[0-9]{8}$ ]] || return 1
    printf '%s-%s-%s\n' "${value:0:4}" "${value:4:2}" "${value:6:2}"
}

RECREATE_REPLAY_START_DATE="$(compact_timestamp_to_iso_date "$db_range_start")" || {
    echo "[FAIL] invalid replay range_start in PostgreSQL metadata: $db_range_start"
    exit 1
}
RECREATE_REPLAY_END_DATE="$(compact_timestamp_to_iso_date "$db_range_end")" || {
    echo "[FAIL] invalid replay range_end in PostgreSQL metadata: $db_range_end"
    exit 1
}

export REPLAY_START_DATE="$RECREATE_REPLAY_START_DATE"
export REPLAY_END_DATE="$RECREATE_REPLAY_END_DATE"
echo "[PASS] recreate will preserve durable replay range: $REPLAY_START_DATE .. $REPLAY_END_DATE"

echo "[INFO] injecting hard replay-controller crash/recreate while execution is pending"

docker kill --signal KILL "$controller_id" >/dev/null 2>&1 || true
docker rm -f "$controller_id" >/dev/null 2>&1 || true

"${COMPOSE[@]}" up -d --no-deps --no-build replay-controller >/dev/null
new_controller_id="$("${COMPOSE[@]}" ps -q replay-controller)"
[[ -n "$new_controller_id" && "$new_controller_id" != "$controller_id" ]] || {
    echo "[FAIL] replay-controller was not recreated with a new container id"
    exit 1
}
echo "[PASS] replay-controller container recreated: $new_controller_id"

recovery_seen=0
for _ in $(seq 1 240); do
    if docker logs "$new_controller_id" 2>&1 | grep -Eq \
        'event=replay_recovery_completed recovered_decisions=[1-9][0-9]* recovered_executions=[1-9][0-9]*'; then
        recovery_seen=1
        break
    fi
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        break
    fi
    sleep 0.25
done
[[ "$recovery_seen" == "1" ]] || {
    echo "[FAIL] recreated replay-controller did not report non-empty PostgreSQL recovery"
    docker logs --tail=160 "$new_controller_id" || true
    exit 1
}

decision_phase_seen=0
for _ in $(seq 1 240); do
    if docker logs "$new_controller_id" 2>&1 | grep -Fq 'event=recovered_decision_barrier'; then
        decision_phase_seen=1
        break
    fi
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        break
    fi
    sleep 0.25
done
[[ "$decision_phase_seen" == "1" ]] || {
    echo "[FAIL] replay-controller did not resume the persisted decision / WAIT_EXECUTION phase"
    docker logs --tail=160 "$new_controller_id" || true
    exit 1
}

echo "[PASS] recreated controller recovered completed prefix + in-flight decision phase"

"${COMPOSE[@]}" unpause order-planner >/dev/null
planner_paused=0
echo "[INFO] order-planner resumed; pending execution may now complete"

set +e
wait "$HARNESS_PID"
HARNESS_STATUS=$?
set -e
if [[ "$HARNESS_STATUS" != "0" ]]; then
    echo "[FAIL] historical comparator harness failed after replay-controller restart"
    cat "$HARNESS_STDOUT" || true
    exit "$HARNESS_STATUS"
fi

LOG_DIR="$(awk -F': ' '/^LOGS[[:space:]]*: /{print $2; exit}' "$HARNESS_STDOUT")"
if [[ -z "$LOG_DIR" || ! -d "$LOG_DIR" ]]; then
    echo "[FAIL] could not resolve historical run log directory"
    cat "$HARNESS_STDOUT" || true
    exit 1
fi

CONTROLLER_LOG="$LOG_DIR/services/replay-controller.log"
COMPARE_LOG="$LOG_DIR/05_distributed_fast_compare.log"
[[ -f "$CONTROLLER_LOG" ]] || { echo "[FAIL] captured replay-controller log missing: $CONTROLLER_LOG"; exit 1; }
[[ -f "$COMPARE_LOG" ]] || { echo "[FAIL] comparator log missing: $COMPARE_LOG"; exit 1; }

grep -Fq 'DISTRIBUTED_FAST_COMPARE: PASS' "$COMPARE_LOG" || {
    echo "[FAIL] restarted distributed path diverged from fast path"
    cat "$COMPARE_LOG"
    exit 1
}

grep -Eq 'event=replay_recovery_completed recovered_decisions=[1-9][0-9]* recovered_executions=[1-9][0-9]*' "$CONTROLLER_LOG" || {
    echo "[FAIL] final controller logs do not contain durable recovery"
    cat "$CONTROLLER_LOG"
    exit 1
}

grep -Fq 'event=recovered_decision_barrier' "$CONTROLLER_LOG" || {
    echo "[FAIL] final controller logs do not prove mid-phase decision recovery"
    cat "$CONTROLLER_LOG"
    exit 1
}

grep -Fq '[REPLAY] finished cycles=90' "$CONTROLLER_LOG" || {
    echo "[FAIL] recreated controller did not preserve total replay-cycle count"
    cat "$CONTROLLER_LOG"
    exit 1
}

echo "[PASS] replay resumed from the exact durable phase after container recreation"
echo "[PASS] total cycle count remained exactly 90"
echo "[PASS] restart replay remained bit-for-bit aligned with fast Decision/Execution reference"
echo

echo "============================================================"
echo "STEP 33B RESULT: PASS"
echo "Replay-controller was killed + deleted + recreated mid-WAIT_EXECUTION."
echo "Completed barriers and the in-flight decision were recovered from PostgreSQL."
echo "No completed cycle was replayed and no cycle was skipped."
echo "Distributed == fast remained exact after recovery."
echo "Logs: $LOG_DIR"
echo "============================================================"
