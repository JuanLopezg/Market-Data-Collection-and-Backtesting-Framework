#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY="$ROOT/deploy/distributed_replay"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
HISTORICAL_DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
LOG_ROOT="$ROOT/validation/logs/restart_remaining_services"
KILL_AFTER_CYCLES="${REMAINING_RESTART_KILL_AFTER_CYCLES:-45}"
COMPOSE_FILE="$DEPLOY/docker-compose.yml"

mkdir -p "$LOG_ROOT"

ACTIVE_HARNESS_PID=""

stop_active_harness() {
    local pid="${ACTIVE_HARNESS_PID:-}"
    [[ -n "$pid" ]] || return 0
    if kill -0 "$pid" 2>/dev/null; then
        echo "[INFO] stopping active historical replay harness pid=$pid"
        kill -TERM "$pid" 2>/dev/null || true
        for _ in $(seq 1 40); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.25
        done
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    ACTIVE_HARNESS_PID=""
}

trap stop_active_harness EXIT INT TERM

stop_stale_project_harnesses() {
    local project="$1"
    local pid
    while read -r pid; do
        [[ -n "$pid" ]] || continue
        [[ "$pid" == "$$" ]] && continue
        echo "[INFO] stopping stale historical harness for project=$project pid=$pid"
        kill -TERM "$pid" 2>/dev/null || true
    done < <(pgrep -f "run_historical_compare\.sh.*--project[[:space:]]+$project([[:space:]]|$)" 2>/dev/null || true)

    # Give any matched child time to run its own EXIT cleanup/preservation logic.
    sleep 0.5

    while read -r pid; do
        [[ -n "$pid" ]] || continue
        [[ "$pid" == "$$" ]] && continue
        kill -KILL "$pid" 2>/dev/null || true
    done < <(pgrep -f "run_historical_compare\.sh.*--project[[:space:]]+$project([[:space:]]|$)" 2>/dev/null || true)
}

[[ -x "$HARNESS" ]] || { echo "[FAIL] historical comparator harness missing: $HARNESS" >&2; exit 1; }
[[ -f "$HISTORICAL_DATA" ]] || { echo "[FAIL] historical data missing: $HISTORICAL_DATA" >&2; exit 1; }
[[ "$KILL_AFTER_CYCLES" =~ ^[0-9]+$ ]] && (( KILL_AFTER_CYCLES >= 5 && KILL_AFTER_CYCLES < 90 )) || {
    echo "[FAIL] REMAINING_RESTART_KILL_AFTER_CYCLES must be between 5 and 89" >&2
    exit 2
}

allowed=(portfolio-risk order-planner execution-state exchange-gateway market-data)
if (( $# == 0 )); then
    requested=("${allowed[@]}")
else
    requested=("$@")
fi

is_allowed() {
    local candidate="$1"
    local value
    for value in "${allowed[@]}"; do
        [[ "$candidate" == "$value" ]] && return 0
    done
    return 1
}

for service in "${requested[@]}"; do
    is_allowed "$service" || {
        echo "[FAIL] unsupported service '$service'" >&2
        echo "Allowed: ${allowed[*]}" >&2
        exit 2
    }
done

pg_scalar() {
    local project="$1" sql="$2"
    docker compose -p "$project" -f "$COMPOSE_FILE" exec -T postgres \
        psql -U algotrading -d algotrading -Atqc "$sql" 2>/dev/null | tr -d '\r'
}

run_case() {
    local service="$1" index="$2"
    local safe="${service//-/_}"
    local project="algotrading_validation_restart_${safe}"
    local nats_port=$((54260 + index))
    local monitor_port=$((58260 + index))
    local postgres_port=$((55460 + index))
    local stamp
    stamp="$(date +%Y%m%d_%H%M%S)"
    local case_root="$LOG_ROOT/$safe"
    local harness_stdout="$case_root/${stamp}_harness.log"
    local compose=(docker compose -p "$project" -f "$COMPOSE_FILE")
    local portfolio_mode="equal-weight"
    [[ "$service" == "portfolio-risk" ]] && portfolio_mode="vol-target"
    local portfolio_config="/opt/algotrading/config/portfolio/pure_rsi_equal_weight.json"
    [[ "$portfolio_mode" == "vol-target" ]] && portfolio_config="/opt/algotrading/config/portfolio/pure_rsi_vol_target.json"
    mkdir -p "$case_root"

    export HISTORICAL_KEEP_ON_FAILURE=1
    export HISTORICAL_DATA_PATH="$HISTORICAL_DATA"
    export REPLAY_NATS_PORT="$nats_port"
    export REPLAY_NATS_MONITOR_PORT="$monitor_port"
    export REPLAY_POSTGRES_PORT="$postgres_port"
    # The historical child harness exports this for its own Compose process, but
    # this parent script performs the hard delete/recreate. Preserve the exact
    # portfolio config here too, otherwise a recreated PortfolioRisk falls back
    # to EqualWeight while PostgreSQL contains a VolTarget checkpoint.
    export REPLAY_PORTFOLIO_CONFIG="$portfolio_config"

    # A failed child run is intentionally preserved for diagnostics. Before a
    # NEW invocation of this case, however, the parent must remove that old
    # topology itself. Otherwise it can observe old containers/logs before the
    # new child reaches its own `compose down`, producing a mixed-generation
    # restart test.
    stop_stale_project_harnesses "$project"
    "${compose[@]}" down -v --remove-orphans >/dev/null 2>&1 || true
    echo "[PASS] clean preflight topology: project=$project"

    echo
    echo "============================================================"
    echo "STEP 33D — HARD RESTART AUDIT: $service"
    echo "Window       : measured 2021-03-01 + 60 days, warmup 30 (90 cycles)"
    echo "Crash after  : >= $KILL_AFTER_CYCLES execution-complete barriers"
    echo "Project      : $project"
    echo "Portfolio    : $portfolio_mode"
    echo "Harness log  : $harness_stdout"
    echo "============================================================"

    local args=(
        --historical-data "$HISTORICAL_DATA"
        --start-date 2021-03-01
        --days 60
        --warmup-days 30
        --portfolio-mode "$portfolio_mode"
        --project "$project"
        --nats-port "$nats_port"
        --nats-monitor-port "$monitor_port"
        --postgres-port "$postgres_port"
        --log-root "$case_root/runs"
        --require-trading
    )
    if (( index == 0 )) && [[ "${RESTART_SKIP_RUNTIME_BUILD:-0}" != "1" ]]; then
        args+=(--force-runtime-build)
    fi

    set +e
    bash "$HARNESS" "${args[@]}" >"$harness_stdout" 2>&1 &
    local harness_pid=$!
    ACTIVE_HARNESS_PID="$harness_pid"
    set -e
    echo "[INFO] historical replay harness pid=$harness_pid"

    local container_id=""
    for _ in $(seq 1 1200); do
        if ! kill -0 "$harness_pid" 2>/dev/null; then
            echo "[FAIL] harness exited before $service became available"
            cat "$harness_stdout" || true
            wait "$harness_pid" || true
            return 1
        fi
        container_id="$("${compose[@]}" ps -q "$service" 2>/dev/null || true)"
        [[ -n "$container_id" ]] && break
        sleep 0.25
    done
    [[ -n "$container_id" ]] || { echo "[FAIL] $service container not found"; return 1; }
    echo "[PASS] $service container started: $container_id"

    local completed=0
    for _ in $(seq 1 2400); do
        if ! kill -0 "$harness_pid" 2>/dev/null; then
            echo "[FAIL] harness exited before restart injection"
            cat "$harness_stdout" || true
            wait "$harness_pid" || true
            return 1
        fi
        completed="$("${compose[@]}" logs --no-color replay-controller 2>/dev/null | grep -c '\[REPLAY\] execution-complete' || true)"
        (( completed >= KILL_AFTER_CYCLES )) && break
        sleep 0.25
    done
    (( completed >= KILL_AFTER_CYCLES )) || { echo "[FAIL] replay never reached crash threshold"; return 1; }

    local market_before=0 account_before=0 decision_before=0
    if [[ "$service" == "portfolio-risk" ]]; then
        market_before="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_market_slice_checkpoint WHERE state_key='portfolio-risk';" || true)"
        account_before="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_account_snapshot_checkpoint_v2 WHERE state_key='portfolio-risk';" || true)"
        decision_before="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_decision_checkpoint WHERE state_key='portfolio-risk';" || true)"
        [[ "$market_before" =~ ^[0-9]+$ && "$account_before" =~ ^[0-9]+$ && "$decision_before" =~ ^[0-9]+$ ]] || {
            echo "[FAIL] portfolio-risk checkpoint tables could not be read"
            return 1
        }
        (( market_before > 0 && account_before > 0 && decision_before > 0 )) || {
            echo "[FAIL] portfolio-risk durable checkpoint is empty: market=$market_before account=$account_before decisions=$decision_before"
            return 1
        }
        echo "[PASS] portfolio-risk durable checkpoint non-empty: market=$market_before account=$account_before decisions=$decision_before"
    fi

    echo "[INFO] injecting hard $service crash/recreate at completed_cycles=$completed"
    docker kill --signal KILL "$container_id" >/dev/null 2>&1 || true
    docker rm -f "$container_id" >/dev/null 2>&1 || true
    "${compose[@]}" up -d --no-deps --no-build "$service" >/dev/null

    local new_id
    new_id="$("${compose[@]}" ps -q "$service")"
    [[ -n "$new_id" && "$new_id" != "$container_id" ]] || {
        echo "[FAIL] $service container was not recreated with a new container id"
        return 1
    }
    echo "[PASS] $service container recreated: $new_id"

    local ready=0
    local execution_state_restore_observed=0
    local execution_state_reconciliation_clean_observed=0

    if [[ "$service" == "portfolio-risk" ]]; then
        # Recovery correctness is proven below from durable state + resumed decisions +
        # final distributed==fast. Do not make one log line a correctness oracle.
        local recreated_cmd
        recreated_cmd="$(docker inspect --format '{{json .Config.Cmd}}' "$new_id" 2>/dev/null || true)"
        grep -Fq "$portfolio_config" <<<"$recreated_cmd" || {
            echo "[FAIL] recreated portfolio-risk did not preserve its exact portfolio config"
            echo "expected config: $portfolio_config"
            echo "container cmd  : $recreated_cmd"
            return 1
        }
        echo "[PASS] recreated portfolio-risk preserved exact config: $portfolio_config"

        # Give Docker/on-failure a short window to expose an immediate fatal/restart loop.
        sleep 1
        local running restart_count
        running="$(docker inspect --format '{{.State.Running}}' "$new_id" 2>/dev/null || true)"
        restart_count="$(docker inspect --format '{{.RestartCount}}' "$new_id" 2>/dev/null || echo 0)"
        [[ "$running" == "true" ]] || {
            echo "[FAIL] recreated portfolio-risk is not running"
            docker logs --tail=200 "$new_id" 2>&1 || true
            return 1
        }
        echo "[PASS] recreated portfolio-risk process is running (restart_count=$restart_count)"
        ready=1
    else
        for _ in $(seq 1 240); do
            local logs
            logs="$(docker logs "$new_id" 2>&1 || true)"
            case "$service" in
                execution-state)
                    if grep -Fq 'event=reconciliation_blocked' <<<"$logs"; then
                        echo "[FAIL] recreated execution-state entered reconciliation_blocked"
                        grep -F 'event=reconciliation_blocked' <<<"$logs" || true
                        return 1
                    fi
                    grep -Fq 'event=state_restored' <<<"$logs" &&
                        execution_state_restore_observed=1
                    grep -Fq 'event=reconciliation_clean' <<<"$logs" &&
                        execution_state_reconciliation_clean_observed=1
                    if (( execution_state_restore_observed == 1 &&
                          execution_state_reconciliation_clean_observed == 1 )); then
                        ready=1
                    fi
                    ;;
                *)
                    grep -Fq 'event=service_ready' <<<"$logs" && ready=1
                    ;;
            esac
            (( ready == 1 )) && break
            if ! kill -0 "$harness_pid" 2>/dev/null; then break; fi
            sleep 0.25
        done
    fi
    (( ready == 1 )) || {
        echo "[FAIL] recreated $service did not reach its expected recovery/ready state"
        if [[ "$service" == "execution-state" ]]; then
            echo "restore_observed=$execution_state_restore_observed reconciliation_clean_observed=$execution_state_reconciliation_clean_observed"
        fi
        docker logs --tail=200 "$new_id" 2>&1 || true
        return 1
    }

    if [[ "$service" == "execution-state" ]]; then
        echo "[PASS] recreated execution-state startup proved PostgreSQL restore"
        echo "[PASS] recreated execution-state startup proved clean reconciliation"
    fi

    if [[ "$service" == "portfolio-risk" ]]; then
        local market_after account_after decision_after
        market_after="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_market_slice_checkpoint WHERE state_key='portfolio-risk';")"
        account_after="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_account_snapshot_checkpoint_v2 WHERE state_key='portfolio-risk';")"
        decision_after="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_decision_checkpoint WHERE state_key='portfolio-risk';")"
        (( market_after >= market_before && account_after >= account_before && decision_after >= decision_before )) || {
            echo "[FAIL] portfolio-risk durable prefix regressed after recreate"
            echo "before market=$market_before account=$account_before decisions=$decision_before"
            echo "after  market=$market_after account=$account_after decisions=$decision_after"
            return 1
        }
        echo "[PASS] recreated portfolio-risk sees non-regressed durable prefix: market=$market_after account=$account_after decisions=$decision_after"
    fi

    local target=$((completed + 5))
    (( target > 89 )) && target=89
    local after="$completed"
    for _ in $(seq 1 1600); do
        if ! kill -0 "$harness_pid" 2>/dev/null; then break; fi
        after="$("${compose[@]}" logs --no-color replay-controller 2>/dev/null | grep -c '\[REPLAY\] execution-complete' || true)"
        (( after >= target )) && break
        sleep 0.25
    done
    (( after >= target )) || {
        echo "[FAIL] replay did not advance after $service recreate: before=$completed after=$after"
        docker logs --tail=220 "$new_id" 2>&1 || true
        return 1
    }
    echo "[PASS] replay advanced after $service recreate: cycles=$completed -> $after"

    if [[ "$service" == "portfolio-risk" ]]; then
        local market_progress account_progress decision_progress recovery_logs
        market_progress="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_market_slice_checkpoint WHERE state_key='portfolio-risk';")"
        account_progress="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_account_snapshot_checkpoint_v2 WHERE state_key='portfolio-risk';")"
        decision_progress="$(pg_scalar "$project" "SELECT count(*) FROM portfolio_risk_decision_checkpoint WHERE state_key='portfolio-risk';")"

        (( market_progress >= market_before && account_progress >= account_before && decision_progress > decision_before )) || {
            echo "[FAIL] recreated portfolio-risk did not append new durable decisions after recovery"
            echo "before   market=$market_before account=$account_before decisions=$decision_before"
            echo "progress market=$market_progress account=$account_progress decisions=$decision_progress"
            docker logs --tail=240 "$new_id" 2>&1 || true
            return 1
        }
        echo "[PASS] recreated portfolio-risk appended durable decisions after recovery: decisions=$decision_before -> $decision_progress"

        recovery_logs="$(docker logs "$new_id" 2>&1 || true)"
        if grep -Fq 'event=portfolio_risk_recovery_completed' <<<"$recovery_logs"; then
            echo "[PASS] recreated portfolio-risk emitted recovery marker"
        else
            echo "[INFO] recovery marker not used as oracle; durable resumed decisions + final exact comparator are authoritative"
        fi
    fi

    set +e
    wait "$harness_pid"
    local harness_status=$?
    ACTIVE_HARNESS_PID=""
    set -e
    if [[ "$harness_status" != "0" ]]; then
        echo "[FAIL] historical comparator harness failed after $service restart"
        cat "$harness_stdout" || true
        return "$harness_status"
    fi

    local log_dir
    log_dir="$(awk -F': ' '/^LOGS[[:space:]]*: /{print $2; exit}' "$harness_stdout")"
    [[ -n "$log_dir" && -d "$log_dir" ]] || {
        echo "[FAIL] could not resolve run log directory for $service"
        return 1
    }
    local compare_log="$log_dir/05_distributed_fast_compare.log"
    local service_log="$log_dir/services/$service.log"
    local execution_log="$log_dir/services/execution-state.log"
    grep -Fq 'DISTRIBUTED_FAST_COMPARE: PASS' "$compare_log" || {
        echo "[FAIL] restarted distributed path diverged from fast path for $service"
        cat "$compare_log"
        return 1
    }
    [[ -f "$service_log" ]] || { echo "[FAIL] captured service log missing: $service_log"; return 1; }

    if [[ -f "$execution_log" ]] && grep -Fq 'event=reconciliation_blocked' "$execution_log"; then
        echo "[FAIL] reconciliation was blocked after $service restart"
        grep -F 'event=reconciliation_blocked' "$execution_log" || true
        return 1
    fi

    if [[ "$service" == "execution-state" ]]; then
        [[ -f "$execution_log" ]] || {
            echo "[FAIL] captured execution-state log is missing"
            return 1
        }
        (( execution_state_restore_observed == 1 )) || {
            echo "[FAIL] recreated execution-state startup did not prove PostgreSQL restore"
            return 1
        }
        (( execution_state_reconciliation_clean_observed == 1 )) || {
            echo "[FAIL] recreated execution-state startup did not prove clean reconciliation"
            return 1
        }
        echo "[PASS] recreated execution-state restored PostgreSQL state and reconciled cleanly"
    fi

    echo "[PASS] $service hard restart remained bit-for-bit aligned with fast reference"
    echo "[PASS] no reconciliation block observed"
    echo "[PASS] STEP 33D service case: $service"
    echo "Logs: $log_dir"
}

index=0
for service in "${requested[@]}"; do
    if ! run_case "$service" "$index"; then
        echo
        echo "[FAIL] STEP 33D stopped at service: $service"
        echo "[INFO] inspect preserved topology with docker compose using the project shown above."
        exit 1
    fi
    index=$((index + 1))
done

echo
echo "============================================================"
echo "STEP 33D RESULT: PASS"
echo "Audited/restarted services: ${requested[*]}"
echo "PortfolioRisk durable rolling/account/decision state recovered from PostgreSQL."
echo "OrderPlanner, ExchangeGateway and MarketData proved recreate-safe under their stateless/reconstructible contracts."
echo "ExecutionState restored PostgreSQL authority and completed clean exchange reconciliation."
echo "Every case remained distributed == fast exact."
echo "============================================================"
