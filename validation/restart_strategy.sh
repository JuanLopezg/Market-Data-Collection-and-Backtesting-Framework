#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY="$ROOT/deploy/distributed_replay"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
HISTORICAL_DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
PROJECT="algotrading_validation_strategy_restart"
NATS_PORT="${STRATEGY_RESTART_NATS_PORT:-54252}"
NATS_MONITOR_PORT="${STRATEGY_RESTART_NATS_MONITOR_PORT:-58252}"
POSTGRES_PORT="${STRATEGY_RESTART_POSTGRES_PORT:-55452}"
KILL_AFTER_CYCLES="${STRATEGY_RESTART_KILL_AFTER_CYCLES:-45}"
LOG_ROOT="$ROOT/validation/logs/strategy_restart"
STAMP="$(date +%Y%m%d_%H%M%S)"
HARNESS_STDOUT="$LOG_ROOT/${STAMP}_harness.log"
COMPOSE=(docker compose -p "$PROJECT" -f "$DEPLOY/docker-compose.yml")

mkdir -p "$LOG_ROOT"

[[ -x "$HARNESS" ]] || { echo "[FAIL] historical comparator harness missing: $HARNESS" >&2; exit 1; }
[[ -f "$HISTORICAL_DATA" ]] || { echo "[FAIL] historical data missing: $HISTORICAL_DATA" >&2; exit 1; }
[[ "$KILL_AFTER_CYCLES" =~ ^[0-9]+$ ]] && (( KILL_AFTER_CYCLES >= 5 && KILL_AFTER_CYCLES < 90 )) || {
    echo "[FAIL] STRATEGY_RESTART_KILL_AFTER_CYCLES must be between 5 and 89" >&2
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

echo "============================================================"
echo "STEP 33A — STRATEGY HARD RESTART / RECREATE"
echo "Window       : measured 2021-03-01 + 60 days, warmup 30 (90 cycles)"
echo "Crash after  : >= $KILL_AFTER_CYCLES execution-complete barriers"
echo "Project      : $PROJECT"
echo "Harness log  : $HARNESS_STDOUT"
echo "============================================================"

# 90-cycle window crosses the previously useful 2021 trading period while staying short.
# A standalone run forces the runtime build. STEP 33E can build once and reuse it.
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

strategy_id=""
for _ in $(seq 1 1200); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        echo "[FAIL] harness exited before strategy container became available"
        cat "$HARNESS_STDOUT" || true
        wait "$HARNESS_PID" || true
        exit 1
    fi
    strategy_id="$("${COMPOSE[@]}" ps -q strategy 2>/dev/null || true)"
    [[ -n "$strategy_id" ]] && break
    sleep 0.25
done
[[ -n "$strategy_id" ]] || { echo "[FAIL] strategy container not found"; exit 1; }

echo "[PASS] strategy container started: $strategy_id"

completed=0
while (( completed < KILL_AFTER_CYCLES )); do
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        echo "[FAIL] harness exited before restart injection"
        cat "$HARNESS_STDOUT" || true
        wait "$HARNESS_PID" || true
        exit 1
    fi
    completed="$("${COMPOSE[@]}" logs --no-color replay-controller 2>/dev/null | grep -c '\[REPLAY\] execution-complete' || true)"
    sleep 0.25
done

echo "[INFO] injecting hard Strategy crash/recreate at completed_cycles=$completed"

pg_scalar() {
    "${COMPOSE[@]}" exec -T postgres \
        psql -U algotrading -d algotrading -Atqc "$1" 2>/dev/null | tr -d '\r'
}

# Prove durable state exists BEFORE destroying Strategy. 33A previously used a log
# line as the early gate; that can false-negative if Compose/log capture changes.
checkpoint_rows_before="$(pg_scalar "SELECT count(*) FROM strategy_market_slice_checkpoint WHERE state_key='strategy-service';" || true)"
checkpoint_max_before="$(pg_scalar "SELECT COALESCE(max(timestamp),0) FROM strategy_market_slice_checkpoint WHERE state_key='strategy-service';" || true)"
[[ "$checkpoint_rows_before" =~ ^[0-9]+$ ]] || {
    echo "[FAIL] could not read Strategy checkpoint row count before crash"
    docker logs --tail=160 "$strategy_id" 2>&1 || true
    exit 1
}
(( checkpoint_rows_before > 0 )) || {
    echo "[FAIL] Strategy had no durable PostgreSQL checkpoints before crash"
    docker logs --tail=160 "$strategy_id" 2>&1 || true
    exit 1
}
echo "[PASS] pre-crash Strategy checkpoint is non-empty: rows=$checkpoint_rows_before max_timestamp=$checkpoint_max_before"

# Remove the whole container, not merely the process. This proves recovery is not
# accidentally relying on the old container writable layer.
docker kill --signal KILL "$strategy_id" >/dev/null 2>&1 || true
docker rm -f "$strategy_id" >/dev/null 2>&1 || true

"${COMPOSE[@]}" up -d --no-deps --no-build strategy >/dev/null
new_strategy_id="$("${COMPOSE[@]}" ps -q strategy)"
[[ -n "$new_strategy_id" && "$new_strategy_id" != "$strategy_id" ]] || {
    echo "[FAIL] strategy container was not recreated with a new container id"
    exit 1
}
echo "[PASS] strategy container recreated: $new_strategy_id"

# PostgreSQL is the correctness source for restart state. The recreated container must
# see at least the complete pre-crash durable prefix; it may already append a pending
# slice before this check runs.
checkpoint_rows_after_recreate=""
for _ in $(seq 1 240); do
    checkpoint_rows_after_recreate="$(pg_scalar "SELECT count(*) FROM strategy_market_slice_checkpoint WHERE state_key='strategy-service';" || true)"
    if [[ "$checkpoint_rows_after_recreate" =~ ^[0-9]+$ ]] && (( checkpoint_rows_after_recreate >= checkpoint_rows_before )); then
        break
    fi
    if ! kill -0 "$HARNESS_PID" 2>/dev/null; then
        break
    fi
    sleep 0.25
done
[[ "$checkpoint_rows_after_recreate" =~ ^[0-9]+$ ]] && (( checkpoint_rows_after_recreate >= checkpoint_rows_before )) || {
    echo "[FAIL] durable Strategy checkpoint prefix was not visible after recreate"
    echo "[INFO] before=$checkpoint_rows_before after=${checkpoint_rows_after_recreate:-unavailable}"
    docker logs --tail=160 "$new_strategy_id" 2>&1 || true
    exit 1
}
echo "[PASS] recreated Strategy sees durable checkpoint prefix: rows=$checkpoint_rows_after_recreate"

# Keep the recovery log as useful diagnostics, but do not make one logger line the
# sole correctness oracle. The hard proof is: durable prefix exists + replay advances +
# new checkpoints appear + exact distributed-vs-fast comparison passes.
recovery_seen=0
for _ in $(seq 1 80); do
    if docker logs "$new_strategy_id" 2>&1 | grep -Eq 'event=strategy_recovery_completed recovered_slices=[1-9][0-9]*'; then
        recovery_seen=1
        break
    fi
    sleep 0.25
done
if [[ "$recovery_seen" == "1" ]]; then
    echo "[PASS] recreated Strategy logged non-empty PostgreSQL recovery"
else
    echo "[WARN] recovery log marker not observed; continuing with state/economic correctness gates"
    docker logs --tail=80 "$new_strategy_id" 2>&1 || true
fi

# Prove the distributed pipeline actually continues after the NEW Strategy instance.
target_completed=$(( completed + 3 ))
if (( target_completed > 89 )); then
    target_completed=89
fi
completed_after_restart=$completed
for _ in $(seq 1 800); do
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
    echo "[FAIL] replay did not advance after Strategy recreate: before=$completed after=$completed_after_restart"
    docker logs --tail=160 "$new_strategy_id" 2>&1 || true
    exit 1
}
echo "[PASS] replay advanced with recreated Strategy: cycles=$completed -> $completed_after_restart"

checkpoint_rows_after_progress="$(pg_scalar "SELECT count(*) FROM strategy_market_slice_checkpoint WHERE state_key='strategy-service';" || true)"
[[ "$checkpoint_rows_after_progress" =~ ^[0-9]+$ ]] && (( checkpoint_rows_after_progress > checkpoint_rows_before )) || {
    echo "[FAIL] recreated Strategy did not append new durable checkpoints"
    echo "[INFO] before=$checkpoint_rows_before after_progress=${checkpoint_rows_after_progress:-unavailable}"
    docker logs --tail=160 "$new_strategy_id" 2>&1 || true
    exit 1
}
echo "[PASS] recreated Strategy appended durable checkpoints: rows=$checkpoint_rows_before -> $checkpoint_rows_after_progress"

set +e
wait "$HARNESS_PID"
HARNESS_STATUS=$?
set -e
if [[ "$HARNESS_STATUS" != "0" ]]; then
    echo "[FAIL] historical comparator harness failed after strategy restart"
    cat "$HARNESS_STDOUT" || true
    exit "$HARNESS_STATUS"
fi

LOG_DIR="$(awk -F': ' '/^LOGS[[:space:]]*: /{print $2; exit}' "$HARNESS_STDOUT")"
if [[ -z "$LOG_DIR" || ! -d "$LOG_DIR" ]]; then
    echo "[FAIL] could not resolve historical run log directory"
    cat "$HARNESS_STDOUT" || true
    exit 1
fi

STRATEGY_LOG="$LOG_DIR/services/strategy.log"
COMPARE_LOG="$LOG_DIR/05_distributed_fast_compare.log"
[[ -f "$STRATEGY_LOG" ]] || { echo "[FAIL] captured strategy log missing: $STRATEGY_LOG"; exit 1; }
[[ -f "$COMPARE_LOG" ]] || { echo "[FAIL] comparator log missing: $COMPARE_LOG"; exit 1; }

grep -Fq 'DISTRIBUTED_FAST_COMPARE: PASS' "$COMPARE_LOG" || {
    echo "[FAIL] restarted distributed path diverged from fast path"
    cat "$COMPARE_LOG"
    exit 1
}

if grep -Eq 'event=strategy_recovery_completed recovered_slices=[1-9][0-9]*' "$STRATEGY_LOG"; then
    echo "[PASS] captured Strategy log contains non-empty recovery marker"
else
    echo "[WARN] captured aggregate Strategy log omitted recovery marker; direct-container/state gates already passed"
fi

grep -Fq 'event=strategy_intents_published' "$STRATEGY_LOG" || {
    echo "[FAIL] strategy did not resume publishing intents after recovery"
    cat "$STRATEGY_LOG"
    exit 1
}

echo "[PASS] restart replay remained bit-for-bit aligned with fast Decision/Execution reference"
echo "[PASS] strategy resumed publishing after durable recovery"
echo

echo "============================================================"
echo "STEP 33A RESULT: PASS"
echo "Strategy container was killed + deleted + recreated mid-replay."
echo "State was rebuilt only from PostgreSQL-checkpointed released slices."
echo "Distributed == fast remained exact after recovery."
echo "Logs: $LOG_DIR"
echo "============================================================"
