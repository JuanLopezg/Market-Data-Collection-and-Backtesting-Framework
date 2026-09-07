#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY="$ROOT/deploy/distributed_replay"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
COMPOSE_FILE="$DEPLOY/docker-compose.yml"
HISTORICAL_DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
LOG_ROOT="$ROOT/validation/logs/chaos_suite"
STAMP="$(date +%Y%m%d_%H%M%S)"
RUN_ROOT="$LOG_ROOT/$STAMP"
SCOPE="${1:-all}"

case "$SCOPE" in
    all|infra|delivery|execution|safety) ;;
    *)
        echo "Usage: bash validation/chaos_suite.sh [all|infra|delivery|execution|safety]" >&2
        exit 2
        ;;
esac

[[ -x "$HARNESS" ]] || { echo "[FAIL] historical harness missing: $HARNESS" >&2; exit 1; }
[[ -f "$HISTORICAL_DATA" ]] || { echo "[FAIL] historical data missing: $HISTORICAL_DATA" >&2; exit 1; }

mkdir -p "$RUN_ROOT"
export HISTORICAL_DATA_PATH="$HISTORICAL_DATA"
export HISTORICAL_KEEP_ON_FAILURE=1

# This 90-cycle window is already proven to contain meaningful orders/fills.
export REPLAY_START_DATE="2021-01-30"
export REPLAY_END_DATE="2021-04-29"
export REPLAY_BARRIER_TIMEOUT_MS="${REPLAY_BARRIER_TIMEOUT_MS:-120000}"

CURRENT_CASE=""
START_EPOCH="$(date +%s)"
BUILD_REQUIRED=1
[[ "${CHAOS_SKIP_RUNTIME_BUILD:-0}" == "1" ]] && BUILD_REQUIRED=0

cleanup_notice() {
    local code=$?
    if [[ "$code" != "0" ]]; then
        echo
        echo "[FAIL] STEP 34 stopped at: ${CURRENT_CASE:-unknown}"
        echo "[INFO] chaos logs: $RUN_ROOT"
        echo "[INFO] failing child harness prints its preserved topology and cleanup command."
    fi
}
trap cleanup_notice EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

reset_chaos_env() {
    unset ALGOTRADING_CHAOS_DUPLICATE_FILL_ONCE || true
    unset ALGOTRADING_CHAOS_NAK_FILL_AFTER_PERSIST_ONCE || true
    unset ALGOTRADING_CHAOS_STALE_PLAN_ONCE || true
    unset ALGOTRADING_CHAOS_REJECT_ONCE || true
    unset ALGOTRADING_CHAOS_PARTIAL_FILL_ONCE || true
    unset ALGOTRADING_CHAOS_PARTIAL_FILL_FRACTION || true
    unset ALGOTRADING_CHAOS_CRASH_AFTER_CHECKPOINT_ONCE || true
}

project_for() {
    local idx="$1"
    printf 'algotrading_validation_chaos_%02d' "$idx"
}

set_ports() {
    local idx="$1"
    export REPLAY_NATS_PORT=$((54320 + idx))
    export REPLAY_NATS_MONITOR_PORT=$((58320 + idx))
    export REPLAY_POSTGRES_PORT=$((55520 + idx))
}

compose_cmd() {
    local project="$1"
    shift
    docker compose -p "$project" -f "$COMPOSE_FILE" "$@"
}

wait_cycles() {
    local project="$1" target="$2" pid="$3"
    local completed=0
    for _ in $(seq 1 3000); do
        if ! kill -0 "$pid" 2>/dev/null; then
            return 1
        fi
        completed="$(compose_cmd "$project" logs --no-color replay-controller 2>/dev/null | grep -c '\[REPLAY\] execution-complete' || true)"
        if (( completed >= target )); then
            echo "$completed"
            return 0
        fi
        sleep 0.25
    done
    return 1
}

wait_progress() {
    local project="$1" before="$2" target_delta="$3" pid="$4"
    local target=$((before + target_delta))
    wait_cycles "$project" "$target" "$pid"
}

wait_service_running() {
    local project="$1" service="$2"
    for _ in $(seq 1 400); do
        local cid status
        cid="$(compose_cmd "$project" ps -q "$service" 2>/dev/null || true)"
        if [[ -n "$cid" ]]; then
            status="$(docker inspect -f '{{.State.Status}}' "$cid" 2>/dev/null || true)"
            [[ "$status" == "running" ]] && return 0
        fi
        sleep 0.25
    done
    return 1
}

wait_service_restart_count() {
    local project="$1" service="$2" minimum="$3" pid="$4"
    for _ in $(seq 1 1200); do
        local cid restart_count
        cid="$(compose_cmd "$project" ps -q "$service" 2>/dev/null || true)"
        if [[ -n "$cid" ]]; then
            restart_count="$(docker inspect -f '{{.RestartCount}}' "$cid" 2>/dev/null || echo 0)"
            if [[ "$restart_count" =~ ^[0-9]+$ ]] && (( restart_count >= minimum )); then
                echo "$restart_count"
                return 0
            fi
        fi

        # If the harness has exited, do one last observation before giving up.
        if ! kill -0 "$pid" 2>/dev/null; then
            cid="$(compose_cmd "$project" ps -q "$service" 2>/dev/null || true)"
            if [[ -n "$cid" ]]; then
                restart_count="$(docker inspect -f '{{.RestartCount}}' "$cid" 2>/dev/null || echo 0)"
                if [[ "$restart_count" =~ ^[0-9]+$ ]] && (( restart_count >= minimum )); then
                    echo "$restart_count"
                    return 0
                fi
            fi
            return 1
        fi
        sleep 0.25
    done
    return 1
}

wait_postgres_healthy() {
    local project="$1"
    for _ in $(seq 1 400); do
        if compose_cmd "$project" exec -T postgres pg_isready -U algotrading -d algotrading >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

start_harness() {
    local idx="$1" label="$2" skip_compare="$3"
    local project
    project="$(project_for "$idx")"
    set_ports "$idx"

    local case_root="$RUN_ROOT/$label"
    mkdir -p "$case_root"
    local stdout="$case_root/harness.log"

    local args=(
        --historical-data "$HISTORICAL_DATA"
        --start-date 2021-03-01
        --days 60
        --warmup-days 30
        --portfolio-mode equal-weight
        --project "$project"
        --nats-port "$REPLAY_NATS_PORT"
        --nats-monitor-port "$REPLAY_NATS_MONITOR_PORT"
        --postgres-port "$REPLAY_POSTGRES_PORT"
        --log-root "$case_root/runs"
        --require-trading
        --allow-controller-recreate
    )
    if [[ "$skip_compare" == "1" ]]; then
        args+=(--skip-fast-compare)
    fi
    if (( BUILD_REQUIRED == 1 )); then
        args+=(--force-runtime-build)
        BUILD_REQUIRED=0
    fi

    bash "$HARNESS" "${args[@]}" >"$stdout" 2>&1 &
    HARNESS_PID=$!
    HARNESS_PROJECT="$project"
    HARNESS_STDOUT="$stdout"
    HARNESS_CASE_ROOT="$case_root"

    echo "[INFO] $label harness pid=$HARNESS_PID project=$HARNESS_PROJECT"
}

finish_harness() {
    local expect_compare="$1"
    set +e
    wait "$HARNESS_PID"
    local status=$?
    set -e
    if (( status != 0 )); then
        echo "[FAIL] harness failed: $HARNESS_STDOUT"
        cat "$HARNESS_STDOUT" || true
        return "$status"
    fi

    RUN_LOG_DIR="$(awk -F': ' '/^LOGS[[:space:]]*: /{print $2; exit}' "$HARNESS_STDOUT")"
    [[ -n "$RUN_LOG_DIR" && -d "$RUN_LOG_DIR" ]] || {
        echo "[FAIL] could not resolve historical run log dir from $HARNESS_STDOUT"
        return 1
    }

    if [[ "$expect_compare" == "1" ]]; then
        grep -Fq 'DISTRIBUTED_FAST_COMPARE: PASS' "$RUN_LOG_DIR/05_distributed_fast_compare.log" || {
            echo "[FAIL] expected distributed == fast PASS"
            cat "$RUN_LOG_DIR/05_distributed_fast_compare.log" || true
            return 1
        }
    fi
}

run_nats_outage() {
    CURRENT_CASE="34A NATS hard outage/recovery"
    reset_chaos_env
    start_harness 0 nats_outage 0
    wait_service_running "$HARNESS_PROJECT" nats || { echo "[FAIL] NATS never started"; return 1; }
    local before
    before="$(wait_cycles "$HARNESS_PROJECT" 35 "$HARNESS_PID")" || {
        echo "[FAIL] replay never reached NATS outage injection point"; return 1;
    }
    echo "[INFO] killing NATS at completed_cycles=$before"
    compose_cmd "$HARNESS_PROJECT" kill nats >/dev/null
    sleep 2
    compose_cmd "$HARNESS_PROJECT" start nats >/dev/null
    wait_service_running "$HARNESS_PROJECT" nats || { echo "[FAIL] NATS did not recover"; return 1; }
    local after
    after="$(wait_progress "$HARNESS_PROJECT" "$before" 5 "$HARNESS_PID")" || {
        echo "[FAIL] replay did not resume after NATS outage"; return 1;
    }
    echo "[PASS] replay resumed after NATS hard outage: cycles=$before -> $after"
    finish_harness 1
    echo "[PASS] 34A NATS outage preserved exact distributed == fast result"
}

run_postgres_outage() {
    CURRENT_CASE="34A PostgreSQL hard outage/recovery"
    reset_chaos_env
    start_harness 1 postgres_outage 0
    wait_service_running "$HARNESS_PROJECT" postgres || { echo "[FAIL] PostgreSQL never started"; return 1; }
    local before
    before="$(wait_cycles "$HARNESS_PROJECT" 35 "$HARNESS_PID")" || {
        echo "[FAIL] replay never reached PostgreSQL outage injection point"; return 1;
    }
    echo "[INFO] killing PostgreSQL at completed_cycles=$before"
    compose_cmd "$HARNESS_PROJECT" kill postgres >/dev/null
    sleep 2
    compose_cmd "$HARNESS_PROJECT" start postgres >/dev/null
    wait_postgres_healthy "$HARNESS_PROJECT" || { echo "[FAIL] PostgreSQL did not recover"; return 1; }

    # Services using libpq hold process-local PGconn objects. Force a clean process restart
    # after DB recovery; STEP 33 already proves each service reconstructs durable state.
    compose_cmd "$HARNESS_PROJECT" restart \
        strategy portfolio-risk simulated-exchange execution-state replay-controller >/dev/null
    for service in strategy portfolio-risk simulated-exchange execution-state replay-controller; do
        wait_service_running "$HARNESS_PROJECT" "$service" || {
            echo "[FAIL] $service did not return after PostgreSQL recovery"; return 1;
        }
    done

    local after
    after="$(wait_progress "$HARNESS_PROJECT" "$before" 5 "$HARNESS_PID")" || {
        echo "[FAIL] replay did not resume after PostgreSQL outage"; return 1;
    }
    echo "[PASS] replay resumed after PostgreSQL hard outage: cycles=$before -> $after"
    finish_harness 1
    echo "[PASS] 34A PostgreSQL outage + durable service recovery remained exact"
}

run_redelivery_after_persist() {
    CURRENT_CASE="34B fill redelivery after durable persist"
    reset_chaos_env
    export ALGOTRADING_CHAOS_NAK_FILL_AFTER_PERSIST_ONCE=1
    start_harness 2 fill_redelivery 0
    finish_harness 1
    local log="$RUN_LOG_DIR/services/execution-state.log"
    grep -Fq 'event=chaos_fill_nak_after_persist' "$log" || {
        echo "[FAIL] NAK-after-persist hook was not exercised"; return 1;
    }
    grep -Fq 'event=fill_duplicate_ignored' "$log" || {
        echo "[FAIL] redelivered FillID was not observed as an idempotent duplicate"; return 1;
    }
    echo "[PASS] 34B commit-before-ACK redelivery was idempotent and remained exact"
}

run_duplicate_fill() {
    CURRENT_CASE="34B duplicate FillID on distinct transport message"
    reset_chaos_env
    export ALGOTRADING_CHAOS_DUPLICATE_FILL_ONCE=1
    start_harness 3 duplicate_fill 0
    finish_harness 1
    grep -Fq 'event=chaos_duplicate_fill_injected' "$RUN_LOG_DIR/services/exchange-gateway.log" || {
        echo "[FAIL] duplicate fill transport injection was not exercised"; return 1;
    }
    grep -Fq 'event=fill_duplicate_ignored' "$RUN_LOG_DIR/services/execution-state.log" || {
        echo "[FAIL] duplicate FillID was not ignored by execution-state"; return 1;
    }
    echo "[PASS] 34B duplicate transport Fill with same FillID produced no duplicate economic effect"
}

run_stale_plan() {
    CURRENT_CASE="34B stale state_revision replan"
    reset_chaos_env
    export ALGOTRADING_CHAOS_STALE_PLAN_ONCE=1
    start_harness 4 stale_plan 0
    finish_harness 1
    grep -Fq 'event=chaos_stale_plan_injected' "$RUN_LOG_DIR/services/order-planner.log" || {
        echo "[FAIL] stale plan injection was not exercised"; return 1;
    }
    grep -Fq 'event=stale_order_plan' "$RUN_LOG_DIR/services/execution-state.log" || {
        echo "[FAIL] execution-state did not reject/replan the stale revision"; return 1;
    }
    echo "[PASS] 34B stale plan was safely replanned and final result remained exact"
}

run_outbox_crash() {
    CURRENT_CASE="34B crash after exchange checkpoint before publish"
    reset_chaos_env
    export ALGOTRADING_CHAOS_CRASH_AFTER_CHECKPOINT_ONCE=1
    start_harness 5 outbox_crash 0

    wait_service_running "$HARNESS_PROJECT" simulated-exchange || {
        echo "[FAIL] simulated-exchange never started"; return 1;
    }

    # The hook intentionally calls std::_Exit(86) immediately after the durable
    # PostgreSQL transition is committed and before the outbox is published.
    # A logger line immediately before _Exit is not a reliable proof because
    # process-level buffered output may never flush. Observe the crash externally
    # from Docker instead.
    local restart_count
    restart_count="$(
        wait_service_restart_count "$HARNESS_PROJECT" simulated-exchange 1 "$HARNESS_PID"
    )" || {
        echo "[FAIL] simulated-exchange never restarted after crash-after-checkpoint injection"
        return 1
    }
    echo "[PASS] simulated-exchange crash observed externally: restart_count=$restart_count"

    finish_harness 1
    local log="$RUN_LOG_DIR/services/simulated-exchange.log"

    # Definitive checkpoint-boundary proof:
    # after restart PostgreSQL must contain at least one unpublished outbox row.
    grep -Eq \
        'event=simulated_exchange_recovery_completed .*recovered=true .*unpublished_outbox=[1-9][0-9]*' \
        "$log" || {
        echo "[FAIL] recovery did not prove a committed-but-unpublished exchange outbox"
        grep -F 'event=simulated_exchange_recovery_completed' "$log" || true
        return 1
    }

    grep -Fq 'event=durable_outbox_published' "$log" || {
        echo "[FAIL] durable exchange outbox was not published after recovery"; return 1;
    }

    # Diagnostic only. std::_Exit(86) may prevent this last pre-crash logger
    # record from reaching Docker even when the hook executed correctly.
    if grep -Fq 'event=chaos_crash_after_checkpoint' "$log"; then
        echo "[PASS] pre-crash hook log marker captured"
    else
        echo "[INFO] pre-crash hook log marker was not flushed before std::_Exit(86); external/durable proof passed"
    fi

    echo "[PASS] 34B crash after commit recovered durable outbox without economic divergence"
}

run_partial_fill() {
    CURRENT_CASE="34C partial fill path"
    reset_chaos_env
    export ALGOTRADING_CHAOS_PARTIAL_FILL_ONCE=1
    export ALGOTRADING_CHAOS_PARTIAL_FILL_FRACTION=0.40
    start_harness 6 partial_fill 1
    finish_harness 0
    local simlog="$RUN_LOG_DIR/services/simulated-exchange.log"
    local execlog="$RUN_LOG_DIR/services/execution-state.log"
    grep -Fq 'event=chaos_partial_fill_split_observed' "$simlog" || {
        echo "[FAIL] partial-fill split was not exercised"; return 1;
    }
    grep -Fq 'event=fill_applied' "$execlog" || {
        echo "[FAIL] execution-state did not apply partial-fill events"; return 1;
    }
    if grep -Fq 'event=reconciliation_blocked' "$execlog"; then
        echo "[FAIL] partial-fill scenario caused reconciliation block"
        return 1
    fi
    echo "[PASS] 34C partial-fill lifecycle completed without reconciliation failure"
}

run_reject() {
    CURRENT_CASE="34C exchange reject path"
    reset_chaos_env
    export ALGOTRADING_CHAOS_REJECT_ONCE=1
    start_harness 7 reject_once 1
    finish_harness 0
    local simlog="$RUN_LOG_DIR/services/simulated-exchange.log"
    local execlog="$RUN_LOG_DIR/services/execution-state.log"
    grep -Fq 'event=chaos_reject_injected' "$simlog" || {
        echo "[FAIL] reject injection was not exercised"; return 1;
    }
    local rejected_order
    rejected_order="$(sed -n 's/.*event=chaos_reject_injected order_id=\([0-9][0-9]*\).*/\1/p' "$simlog" | head -1)"
    [[ -n "$rejected_order" ]] || { echo "[FAIL] could not resolve rejected OrderID"; return 1; }
    grep -Eq "event=order_update_received order_id=${rejected_order} .*status=6 .*CHAOS_REJECT_ONCE" "$execlog" || {
        echo "[FAIL] execution-state did not observe the injected Rejected lifecycle"; return 1;
    }
    if grep -Eq "event=fill_received .*order_id=${rejected_order}([[:space:]]|$)" "$execlog"; then
        echo "[FAIL] rejected order $rejected_order received a fill"
        return 1
    fi
    if grep -Fq 'event=reconciliation_blocked' "$execlog"; then
        echo "[FAIL] reject scenario caused reconciliation block"
        return 1
    fi
    echo "[PASS] 34C rejected order remained unfilled and replay completed safely"
}

run_reconciliation_mismatch() {
    CURRENT_CASE="34D reconciliation mismatch -> safe block"
    reset_chaos_env
    # This case intentionally creates an unsafe external/local mismatch, so the replay
    # is expected NOT to complete. Use the harness only to create a realistic topology.
    start_harness 8 reconciliation_mismatch 1

    local before
    before="$(wait_cycles "$HARNESS_PROJECT" 35 "$HARNESS_PID")" || {
        echo "[FAIL] replay never reached reconciliation mismatch injection point"; return 1;
    }

    compose_cmd "$HARNESS_PROJECT" pause replay-controller >/dev/null
    echo "[INFO] replay-controller paused at completed_cycles=$before"

    # Corrupt only the simulated exchange's independent durable truth.
    compose_cmd "$HARNESS_PROJECT" exec -T postgres \
        psql -U algotrading -d algotrading -v ON_ERROR_STOP=1 -c \
        "UPDATE simulated_exchange_state SET cash = cash + 123.456 WHERE state_key='simulated-exchange';" \
        >/dev/null

    # Recreate exchange and execution-state so both read their independent durable stores.
    compose_cmd "$HARNESS_PROJECT" rm -sf simulated-exchange execution-state >/dev/null
    compose_cmd "$HARNESS_PROJECT" up -d simulated-exchange execution-state >/dev/null
    wait_service_running "$HARNESS_PROJECT" simulated-exchange || {
        echo "[FAIL] simulated-exchange did not restart after mismatch injection"; return 1;
    }
    wait_service_running "$HARNESS_PROJECT" execution-state || {
        echo "[FAIL] execution-state did not restart after mismatch injection"; return 1;
    }

    local exec_id
    exec_id="$(compose_cmd "$HARNESS_PROJECT" ps -q execution-state)"
    local blocked=0
    for _ in $(seq 1 400); do
        if docker logs "$exec_id" 2>&1 | grep -Fq 'event=reconciliation_blocked'; then
            blocked=1
            break
        fi
        sleep 0.25
    done
    if (( blocked != 1 )); then
        echo "[FAIL] mismatch did not force reconciliation_blocked"
        docker logs --tail=240 "$exec_id" 2>&1 || true
        return 1
    fi

    echo "[PASS] deliberate exchange/local mismatch forced reconciliation_blocked"

    # Capture the intentional safe-failure logs before stopping the child harness.
    docker logs "$exec_id" >"$HARNESS_CASE_ROOT/execution-state-mismatch.log" 2>&1 || true
    compose_cmd "$HARNESS_PROJECT" logs --no-color simulated-exchange \
        >"$HARNESS_CASE_ROOT/simulated-exchange-mismatch.log" 2>&1 || true

    # We intentionally do not let an unsafe topology continue.
    kill -TERM "$HARNESS_PID" 2>/dev/null || true
    set +e
    wait "$HARNESS_PID"
    set -e
    compose_cmd "$HARNESS_PROJECT" down -v --remove-orphans >/dev/null 2>&1 || true
    echo "[PASS] 34D unsafe mismatch did not auto-repair or silently resume"
}

run_selected() {
    case "$SCOPE" in
        all|infra)
            run_nats_outage
            run_postgres_outage
            ;;
    esac
    case "$SCOPE" in
        all|delivery)
            run_redelivery_after_persist
            run_duplicate_fill
            run_stale_plan
            run_outbox_crash
            ;;
    esac
    case "$SCOPE" in
        all|execution)
            run_partial_fill
            run_reject
            ;;
    esac
    case "$SCOPE" in
        all|safety)
            run_reconciliation_mismatch
            ;;
    esac
}

run_selected
reset_chaos_env
CURRENT_CASE=""

END_EPOCH="$(date +%s)"
ELAPSED=$((END_EPOCH - START_EPOCH))

echo
echo "============================================================"
echo "STEP 34 RESULT: PASS"
echo "Scope      : $SCOPE"
echo "Elapsed    : ${ELAPSED}s"
echo "Suite logs : $RUN_ROOT"
echo "Validated  : infra outage recovery, durable redelivery/idempotence,"
echo "             stale-plan protection, outbox crash recovery,"
echo "             partial fill, reject, and reconciliation safe-block"
echo "============================================================"
