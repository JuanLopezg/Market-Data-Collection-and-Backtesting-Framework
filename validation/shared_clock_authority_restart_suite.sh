#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY="$ROOT/deploy/distributed_replay"
COMPOSE_FILE="$DEPLOY/docker-compose.yml"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
SOURCE_GATE="$SCRIPT_DIR/shared_clock_authority_restart_check.py"
CLOCK_AUDIT="$SCRIPT_DIR/clock_audit.sh"
HISTORICAL_DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
LOG_ROOT="$ROOT/validation/logs/clock_authority_restart"
KILL_AFTER_CYCLES="${CLOCK_AUTHORITY_KILL_AFTER_CYCLES:-20}"
MEASURED_DAYS="${CLOCK_AUTHORITY_MEASURED_DAYS:-20}"
WARMUP_DAYS="${CLOCK_AUTHORITY_WARMUP_DAYS:-30}"
BARRIER_TIMEOUT_MS="${CLOCK_AUTHORITY_BARRIER_TIMEOUT_MS:-240000}"
FORCE_RUNTIME_BUILD="${CLOCK_AUTHORITY_FORCE_RUNTIME_BUILD:-1}"

ALL_PHASES=(decision execution)
ACTIVE_HARNESS_PID=""
ACTIVE_PROJECT=""
ACTIVE_BLOCKER=""

usage() {
    cat <<'EOF'
Usage: bash validation/shared_clock_authority_restart_suite.sh [decision|execution ...]

Without arguments both authority-restart phases are validated:
  decision   kill/recreate replay-controller while waiting for DecisionBatch
  execution  kill/recreate replay-controller while waiting for execution completion

Optional environment:
  CLOCK_AUTHORITY_KILL_AFTER_CYCLES    default 20
  CLOCK_AUTHORITY_MEASURED_DAYS        default 20
  CLOCK_AUTHORITY_WARMUP_DAYS          default 30
  CLOCK_AUTHORITY_BARRIER_TIMEOUT_MS   default 240000
  CLOCK_AUTHORITY_FORCE_RUNTIME_BUILD  default 1 (force first-case runtime image rebuild)
EOF
}

contains_phase() {
    local candidate="$1" phase
    for phase in "${ALL_PHASES[@]}"; do
        [[ "$candidate" == "$phase" ]] && return 0
    done
    return 1
}

cleanup_active() {
    local code=$?
    if [[ -n "$ACTIVE_HARNESS_PID" ]] && kill -0 "$ACTIVE_HARNESS_PID" 2>/dev/null; then
        kill -TERM "$ACTIVE_HARNESS_PID" 2>/dev/null || true
        wait "$ACTIVE_HARNESS_PID" 2>/dev/null || true
    fi
    if [[ "$code" != "0" && -n "$ACTIVE_PROJECT" ]]; then
        echo "[INFO] failed topology may be preserved for diagnostics: project=$ACTIVE_PROJECT"
        echo "[INFO] inspect: docker compose -p $ACTIVE_PROJECT -f $COMPOSE_FILE ps -a"
        echo "[INFO] cleanup: docker compose -p $ACTIVE_PROJECT -f $COMPOSE_FILE down -v --remove-orphans"
        [[ -n "$ACTIVE_BLOCKER" ]] && echo "[INFO] blocker may still be paused: $ACTIVE_BLOCKER"
    fi
}
trap cleanup_active EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

[[ -x "$HARNESS" ]] || { echo "[FAIL] historical comparator harness missing: $HARNESS" >&2; exit 1; }
[[ -x "$SOURCE_GATE" ]] || { echo "[FAIL] STEP 35E source gate missing: $SOURCE_GATE" >&2; exit 1; }
[[ -x "$CLOCK_AUDIT" ]] || { echo "[FAIL] clock audit missing: $CLOCK_AUDIT" >&2; exit 1; }
[[ -f "$HISTORICAL_DATA" ]] || { echo "[FAIL] historical data missing: $HISTORICAL_DATA" >&2; exit 1; }
[[ "$KILL_AFTER_CYCLES" =~ ^[0-9]+$ ]] || { echo "[FAIL] CLOCK_AUTHORITY_KILL_AFTER_CYCLES must be an integer" >&2; exit 2; }
[[ "$MEASURED_DAYS" =~ ^[0-9]+$ ]] || { echo "[FAIL] CLOCK_AUTHORITY_MEASURED_DAYS must be an integer" >&2; exit 2; }
[[ "$WARMUP_DAYS" =~ ^[0-9]+$ ]] || { echo "[FAIL] CLOCK_AUTHORITY_WARMUP_DAYS must be an integer" >&2; exit 2; }
[[ "$FORCE_RUNTIME_BUILD" == "0" || "$FORCE_RUNTIME_BUILD" == "1" ]] || { echo "[FAIL] CLOCK_AUTHORITY_FORCE_RUNTIME_BUILD must be 0 or 1" >&2; exit 2; }
(( KILL_AFTER_CYCLES >= 5 )) || { echo "[FAIL] crash threshold must be >= 5 cycles" >&2; exit 2; }
(( MEASURED_DAYS >= 5 && WARMUP_DAYS >= 5 )) || { echo "[FAIL] measured/warmup windows are too small" >&2; exit 2; }
(( KILL_AFTER_CYCLES < MEASURED_DAYS + WARMUP_DAYS - 3 )) || {
    echo "[FAIL] crash threshold leaves too few cycles for post-restart proof" >&2
    exit 2
}

if (( $# == 0 )); then
    REQUESTED=("${ALL_PHASES[@]}")
else
    if [[ "$1" == "--help" || "$1" == "-h" ]]; then usage; exit 0; fi
    REQUESTED=("$@")
fi
for phase in "${REQUESTED[@]}"; do
    contains_phase "$phase" || { echo "[FAIL] unsupported phase: $phase" >&2; usage >&2; exit 2; }
done

mkdir -p "$LOG_ROOT"

echo "============================================================"
echo "STEP 35E — REPLAY AUTHORITY HARD RESTART / DURABLE CLOCK RECOVERY"
echo "============================================================"
python3 "$SOURCE_GATE" --root "$ROOT"
echo
bash "$CLOCK_AUDIT"
echo "[PASS] source architecture + clock audit prerequisites remain clean"

echo "============================================================"
echo "STEP 35E — INCREMENTAL BUILD"
echo "============================================================"
ninja -C "$ROOT/build" -j8
echo "[PASS] full project build before authority restart injection"
echo "[INFO] first-case runtime image rebuild: $([[ "$FORCE_RUNTIME_BUILD" == "1" ]] && echo FORCED || echo REUSE_ALLOWED)"

compact_timestamp_to_iso_date() {
    local value="$1"
    [[ "$value" =~ ^[0-9]{8}$ ]] || return 1
    printf '%s-%s-%s\n' "${value:0:4}" "${value:4:2}" "${value:6:2}"
}

pg_scalar() {
    local project="$1" sql="$2"
    docker compose -p "$project" -f "$COMPOSE_FILE" exec -T postgres \
        psql -U algotrading -d algotrading -Atqc "$sql" 2>/dev/null | tr -d '\r'
}

clock_state_row() {
    local project="$1"
    pg_scalar "$project" \
        "SELECT simulation_id || '|' || logical_time::text || '|' || revision::text || '|' || mode::text || '|' || speed_multiplier::text || '|' || paused::text || '|' || message_id FROM replay_controller_clock_state WHERE state_key='replay-controller';"
}

checkpoint_counts() {
    local project="$1"
    pg_scalar "$project" \
        "SELECT (SELECT count(*) FROM replay_controller_decision_checkpoint WHERE state_key='replay-controller')::text || '|' || (SELECT count(*) FROM replay_controller_execution_checkpoint WHERE state_key='replay-controller')::text;"
}

replay_range_row() {
    local project="$1"
    pg_scalar "$project" \
        "SELECT range_start::text || '|' || range_end::text FROM replay_controller_metadata WHERE state_key='replay-controller';"
}

completed_cycles_for_id() {
    local controller_id="$1"
    docker logs "$controller_id" 2>&1 | grep -c '\[REPLAY\] execution-complete' || true
}

close_release_count_for_id() {
    local controller_id="$1"
    docker logs "$controller_id" 2>&1 | grep -c 'event=market_release_published kind=close' || true
}

open_release_count_for_id() {
    local controller_id="$1"
    docker logs "$controller_id" 2>&1 | grep -c 'event=market_release_published kind=execution_open' || true
}

recovery_log_matches() {
    local line="$1" sim="$2" logical_time="$3" revision="$4"
    [[ -n "$line" ]] &&
    [[ "$line" == *"event=replay_recovery_completed"* ]] &&
    [[ "$line" == *"clock_recovered=true"* ]] &&
    [[ "$line" == *"clock_time=$logical_time"* ]] &&
    [[ "$line" == *"clock_revision=$revision"* ]] &&
    [[ "$line" == *"simulation_id=$sim"* ]]
}

wait_for_container() {
    local project="$1" service="$2" harness_pid="$3"
    local compose=(docker compose -p "$project" -f "$COMPOSE_FILE")
    local id=""
    for _ in $(seq 1 1200); do
        if ! kill -0 "$harness_pid" 2>/dev/null; then
            return 1
        fi
        id="$("${compose[@]}" ps -q "$service" 2>/dev/null || true)"
        if [[ -n "$id" ]]; then
            printf '%s\n' "$id"
            return 0
        fi
        sleep 0.25
    done
    return 1
}

all_followers_running() {
    local project="$1"
    local compose=(docker compose -p "$project" -f "$COMPOSE_FILE")
    local service id running
    for service in market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange; do
        id="$("${compose[@]}" ps -q "$service" 2>/dev/null || true)"
        [[ -n "$id" ]] || return 1
        running="$(docker inspect -f '{{.State.Running}}' "$id" 2>/dev/null || true)"
        [[ "$running" == "true" ]] || return 1
    done
    return 0
}

wait_for_phase_barrier() {
    local project="$1" controller_id="$2" harness_pid="$3" phase="$4"
    local last_state="" last_counts="" stable=0
    local state counts decisions executions closes opens

    # The blocker is already paused. Require ~2 seconds of identical durable state and
    # checkpoint counts so the old authority is definitely waiting at the selected
    # causal barrier rather than merely sampled between two fast logical ticks.
    for _ in $(seq 1 480); do
        kill -0 "$harness_pid" 2>/dev/null || return 1
        state="$(clock_state_row "$project" || true)"
        counts="$(checkpoint_counts "$project" || true)"
        [[ -n "$state" && "$counts" =~ ^[0-9]+\|[0-9]+$ ]] || { sleep 0.25; continue; }
        IFS='|' read -r decisions executions <<<"$counts"
        closes="$(close_release_count_for_id "$controller_id")"
        opens="$(open_release_count_for_id "$controller_id")"

        local phase_ok=0
        if [[ "$phase" == "decision" ]]; then
            # CLOSE(T) has been released but Strategy is paused, so there is no durable
            # DecisionBatch for that close yet. Completed decision/execution prefixes match.
            if (( decisions == executions && closes > decisions )); then
                phase_ok=1
            fi
        else
            # Decision(T) is durable, OPEN(T+1) has been released, and OrderPlanner is
            # paused so execution completion cannot arrive. Exactly one decision is ahead.
            if (( decisions == executions + 1 && opens > executions )); then
                phase_ok=1
            fi
        fi

        if (( phase_ok == 1 )) && [[ "$state" == "$last_state" && "$counts" == "$last_counts" ]]; then
            stable=$((stable + 1))
        else
            stable=0
            last_state="$state"
            last_counts="$counts"
        fi
        if (( stable >= 8 )); then
            printf '%s;%s\n' "$state" "$counts"
            return 0
        fi
        sleep 0.25
    done
    return 1
}

run_case() {
    local phase="$1" index="$2"
    local project="algotrading_clock_authority_restart_${phase}"
    local nats_port=$((54340 + index))
    local monitor_port=$((58340 + index))
    local postgres_port=$((55540 + index))
    local simulation_id="clock-authority-restart-${phase}"
    local blocker="strategy"
    [[ "$phase" == "execution" ]] && blocker="order-planner"

    local stamp
    stamp="$(date +%Y%m%d_%H%M%S)"
    local case_root="$LOG_ROOT/$phase"
    local harness_stdout="$case_root/${stamp}_harness.log"
    local precrash_controller_log="$case_root/${stamp}_precrash_controller.log"
    local recovered_controller_log="$case_root/${stamp}_recovered_controller.log"
    local compose=(docker compose -p "$project" -f "$COMPOSE_FILE")
    mkdir -p "$case_root"

    export HISTORICAL_KEEP_ON_FAILURE=1
    export HISTORICAL_DATA_PATH="$HISTORICAL_DATA"
    export REPLAY_NATS_PORT="$nats_port"
    export REPLAY_NATS_MONITOR_PORT="$monitor_port"
    export REPLAY_POSTGRES_PORT="$postgres_port"
    export REPLAY_PORTFOLIO_CONFIG="/opt/algotrading/config/portfolio/pure_rsi_equal_weight.json"
    export REPLAY_SIMULATION_ID="$simulation_id"
    export REPLAY_BARRIER_TIMEOUT_MS="$BARRIER_TIMEOUT_MS"

    "${compose[@]}" down -v --remove-orphans >/dev/null 2>&1 || true

    echo
    echo "============================================================"
    echo "STEP 35E CASE — replay authority hard restart @ $phase barrier"
    echo "Project       : $project"
    echo "Simulation ID : $simulation_id"
    echo "Blocker       : $blocker"
    echo "Window        : 2021-03-01 + $MEASURED_DAYS measured, $WARMUP_DAYS warmup"
    echo "Restart after : >= $KILL_AFTER_CYCLES completed cycles"
    echo "Harness log   : $harness_stdout"
    echo "============================================================"

    local args=(
        --historical-data "$HISTORICAL_DATA"
        --start-date 2021-03-01
        --days "$MEASURED_DAYS"
        --warmup-days "$WARMUP_DAYS"
        --portfolio-mode equal-weight
        --project "$project"
        --nats-port "$nats_port"
        --nats-monitor-port "$monitor_port"
        --postgres-port "$postgres_port"
        --barrier-timeout-ms "$BARRIER_TIMEOUT_MS"
        --log-root "$case_root/runs"
        --allow-controller-recreate
    )
    if (( index == 0 )) && [[ "$FORCE_RUNTIME_BUILD" == "1" ]]; then
        args+=(--force-runtime-build)
    fi

    set +e
    bash "$HARNESS" "${args[@]}" >"$harness_stdout" 2>&1 &
    local harness_pid=$!
    set -e
    ACTIVE_HARNESS_PID="$harness_pid"
    ACTIVE_PROJECT="$project"
    ACTIVE_BLOCKER=""
    echo "[INFO] historical replay harness pid=$harness_pid"

    local controller_id blocker_id
    controller_id="$(wait_for_container "$project" replay-controller "$harness_pid")" || {
        echo "[FAIL] replay-controller container did not become available"
        cat "$harness_stdout" || true
        return 1
    }
    blocker_id="$(wait_for_container "$project" "$blocker" "$harness_pid")" || {
        echo "[FAIL] blocker container did not become available: $blocker"
        return 1
    }
    echo "[PASS] runtime containers available: controller=$controller_id blocker=$blocker"

    local completed=0
    for _ in $(seq 1 2400); do
        if ! kill -0 "$harness_pid" 2>/dev/null; then
            echo "[FAIL] harness exited before authority restart threshold"
            cat "$harness_stdout" || true
            return 1
        fi
        completed="$(completed_cycles_for_id "$controller_id")"
        (( completed >= KILL_AFTER_CYCLES )) && break
        sleep 0.25
    done
    (( completed >= KILL_AFTER_CYCLES )) || { echo "[FAIL] replay did not reach restart threshold"; return 1; }
    echo "[PASS] replay reached restart threshold: completed_cycles=$completed"

    docker pause "$blocker_id" >/dev/null
    ACTIVE_BLOCKER="$blocker"
    echo "[INFO] paused $blocker; establishing stable $phase barrier"

    local held_bundle
    held_bundle="$(wait_for_phase_barrier "$project" "$controller_id" "$harness_pid" "$phase")" || {
        echo "[FAIL] could not establish stable $phase barrier"
        docker logs --tail=200 "$controller_id" 2>&1 || true
        return 1
    }

    local held_state held_counts
    held_state="${held_bundle%%;*}"
    held_counts="${held_bundle#*;}"
    local held_sim held_time held_revision held_mode held_speed held_paused held_message_id
    IFS='|' read -r held_sim held_time held_revision held_mode held_speed held_paused held_message_id <<<"$held_state"
    local held_decisions held_executions
    IFS='|' read -r held_decisions held_executions <<<"$held_counts"

    [[ "$held_sim" == "$simulation_id" && "$held_time" =~ ^[0-9]+$ && "$held_revision" =~ ^[0-9]+$ && -n "$held_message_id" ]] || {
        echo "[FAIL] malformed/unexpected durable authority state: $held_state"
        return 1
    }
    [[ "$held_paused" == "f" || "$held_paused" == "false" ]] || {
        echo "[FAIL] 35E expects trading clock running while a business-path blocker creates the barrier"
        return 1
    }
    if [[ "$phase" == "decision" ]]; then
        (( held_decisions == held_executions )) || { echo "[FAIL] decision barrier checkpoint shape invalid: $held_counts"; return 1; }
    else
        (( held_decisions == held_executions + 1 )) || { echo "[FAIL] execution barrier checkpoint shape invalid: $held_counts"; return 1; }
    fi
    echo "[PASS] stable authority phase: phase=$phase simulation_id=$held_sim logical_time=$held_time revision=$held_revision decisions=$held_decisions executions=$held_executions"

    docker logs "$controller_id" >"$precrash_controller_log" 2>&1 || true

    local range_row range_start range_end
    range_row="$(replay_range_row "$project")"
    IFS='|' read -r range_start range_end <<<"$range_row"
    export REPLAY_START_DATE
    export REPLAY_END_DATE
    REPLAY_START_DATE="$(compact_timestamp_to_iso_date "$range_start")" || {
        echo "[FAIL] invalid durable range_start: $range_start"; return 1;
    }
    REPLAY_END_DATE="$(compact_timestamp_to_iso_date "$range_end")" || {
        echo "[FAIL] invalid durable range_end: $range_end"; return 1;
    }
    echo "[PASS] controller recreation pinned to durable range: $REPLAY_START_DATE .. $REPLAY_END_DATE"

    local original_restart_count restart_policy
    original_restart_count="$(docker inspect -f '{{.RestartCount}}' "$controller_id" 2>/dev/null || true)"
    [[ "$original_restart_count" =~ ^[0-9]+$ ]] || original_restart_count=0
    restart_policy="$(docker inspect -f '{{.HostConfig.RestartPolicy.Name}}' "$controller_id" 2>/dev/null || true)"
    echo "[INFO] isolating replay-controller SIGKILL from Docker restart policy: current=${restart_policy:-<unknown>} -> no"
    docker update --restart=no "$controller_id" >/dev/null
    [[ "$(docker inspect -f '{{.HostConfig.RestartPolicy.Name}}' "$controller_id" 2>/dev/null || true)" == "no" ]] || {
        echo "[FAIL] could not disable replay-controller restart policy"; return 1;
    }

    docker kill --signal KILL "$controller_id" >/dev/null 2>&1 || true
    local old_running="true"
    for _ in $(seq 1 80); do
        old_running="$(docker inspect -f '{{.State.Running}}' "$controller_id" 2>/dev/null || echo false)"
        [[ "$old_running" == "false" ]] && break
        sleep 0.10
    done
    [[ "$old_running" == "false" ]] || { echo "[FAIL] hard-killed authority did not stop"; return 1; }

    local after_kill_restart_count
    after_kill_restart_count="$(docker inspect -f '{{.RestartCount}}' "$controller_id" 2>/dev/null || true)"
    [[ "$after_kill_restart_count" =~ ^[0-9]+$ ]] || after_kill_restart_count=0
    [[ "$after_kill_restart_count" == "$original_restart_count" ]] || {
        echo "[FAIL] Docker auto-restarted replay-controller during the proof"
        echo "restart_count_before=$original_restart_count restart_count_after=$after_kill_restart_count"
        return 1
    }
    echo "[PASS] replay-controller SIGKILL produced exactly one stopped authority process"

    # The key durability proof: with no authority process alive, PostgreSQL must still hold
    # the exact semantic ClockState and barrier checkpoint shape captured before SIGKILL.
    local offline_state offline_counts
    offline_state="$(clock_state_row "$project")"
    offline_counts="$(checkpoint_counts "$project")"
    [[ "$offline_state" == "$held_state" && "$offline_counts" == "$held_counts" ]] || {
        echo "[FAIL] durable authority state changed/lost while replay-controller was absent"
        echo "before clock=$held_state counts=$held_counts"
        echo "offline clock=$offline_state counts=$offline_counts"
        return 1
    }
    echo "[PASS] clock state remained durable while authority was absent"

    docker rm -f "$controller_id" >/dev/null 2>&1 || true
    "${compose[@]}" up -d --no-deps --no-build replay-controller >/dev/null

    local new_controller_id
    new_controller_id="$("${compose[@]}" ps -q replay-controller 2>/dev/null || true)"
    [[ -n "$new_controller_id" && "$new_controller_id" != "$controller_id" ]] || {
        echo "[FAIL] replay-controller was not recreated with a new container id"; return 1;
    }
    local new_running="false"
    for _ in $(seq 1 160); do
        new_running="$(docker inspect -f '{{.State.Running}}' "$new_controller_id" 2>/dev/null || true)"
        [[ "$new_running" == "true" ]] && break
        sleep 0.25
    done
    [[ "$new_running" == "true" ]] || {
        echo "[FAIL] recreated replay-controller did not reach running state"
        docker logs --tail=200 "$new_controller_id" 2>&1 || true
        return 1
    }
    [[ "$(docker inspect -f '{{.HostConfig.RestartPolicy.Name}}' "$new_controller_id" 2>/dev/null || true)" == "on-failure" ]] || {
        echo "[FAIL] recreated replay-controller did not restore Compose restart:on-failure"; return 1;
    }
    echo "[PASS] replay-controller recreated exactly once: $controller_id -> $new_controller_id"

    local recovery_seen=0 publish_seen=0 phase_seen=0
    local last_recovery_line=""
    for _ in $(seq 1 600); do
        local logs
        logs="$(docker logs "$new_controller_id" 2>&1 || true)"
        last_recovery_line="$(grep -F 'event=replay_recovery_completed' <<<"$logs" | tail -n 1 || true)"
        if recovery_log_matches "$last_recovery_line" "$held_sim" "$held_time" "$held_revision"; then
            recovery_seen=1
        fi
        if grep -Fq "event=clock_state_published simulation_id=$held_sim logical_time=$held_time revision=$held_revision" <<<"$logs"; then
            publish_seen=1
        fi
        if [[ "$phase" == "decision" ]]; then
            if grep -Fq "event=market_release_published kind=close timestamp=$held_time" <<<"$logs"; then
                phase_seen=1
            fi
        else
            if grep -Fq 'event=recovered_decision_barrier' <<<"$logs" && \
               grep -Fq "event=market_release_published kind=execution_open timestamp=$held_time" <<<"$logs"; then
                phase_seen=1
            fi
        fi
        if (( recovery_seen == 1 && publish_seen == 1 && phase_seen == 1 )); then
            break
        fi
        if ! kill -0 "$harness_pid" 2>/dev/null; then
            break
        fi
        sleep 0.25
    done
    docker logs "$new_controller_id" >"$recovered_controller_log" 2>&1 || true

    (( recovery_seen == 1 )) || {
        echo "[FAIL] recreated authority did not report exact durable clock recovery"
        echo "[INFO] replay_recovery_completed line: ${last_recovery_line:-<none>}"
        echo "[INFO] expected fields: clock_recovered=true clock_time=$held_time clock_revision=$held_revision simulation_id=$held_sim"
        tail -n 220 "$recovered_controller_log" || true
        return 1
    }
    (( publish_seen == 1 )) || {
        echo "[FAIL] recreated authority did not re-emit the recovered ClockState"
        tail -n 220 "$recovered_controller_log" || true
        return 1
    }
    (( phase_seen == 1 )) || {
        echo "[FAIL] recreated authority did not resume the exact $phase barrier phase"
        tail -n 220 "$recovered_controller_log" || true
        return 1
    }
    echo "[PASS] exact durable clock recovered: simulation_id=$held_sim logical_time=$held_time revision=$held_revision"
    echo "[PASS] exact $phase barrier phase recovered"

    # Keep the blocker paused long enough to prove the new controller cannot manufacture
    # progress merely because it restarted. Same logical time/revision and checkpoint counts
    # must remain stable while the causal dependency is still unavailable.
    sleep 2
    local post_recovery_state post_recovery_counts
    post_recovery_state="$(clock_state_row "$project")"
    post_recovery_counts="$(checkpoint_counts "$project")"
    [[ "$post_recovery_state" == "$held_state" && "$post_recovery_counts" == "$held_counts" ]] || {
        echo "[FAIL] authority advanced or rewrote durable state before causal blocker was released"
        echo "held clock=$held_state counts=$held_counts"
        echo "after restart clock=$post_recovery_state counts=$post_recovery_counts"
        return 1
    }
    echo "[PASS] authority did not advance while causal blocker remained paused"

    all_followers_running "$project" || {
        echo "[FAIL] one or more clock followers stopped during authority recovery"; return 1;
    }
    local combined_logs
    combined_logs="$("${compose[@]}" logs --no-color market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange 2>/dev/null || true)"
    if grep -Eq 'event=clock_state_rejected|logical clock attempted to move backwards|simulation_id_mismatch|same_revision_conflict' <<<"$combined_logs"; then
        echo "[FAIL] follower clock identity/monotonicity error observed during authority restart"
        grep -E 'event=clock_state_rejected|logical clock attempted to move backwards|simulation_id_mismatch|same_revision_conflict' <<<"$combined_logs" | tail -n 80 || true
        return 1
    fi
    echo "[PASS] all seven followers stayed live with no clock identity/monotonicity rejection"

    docker unpause "$blocker_id" >/dev/null
    ACTIVE_BLOCKER=""
    echo "[INFO] unpaused $blocker; normal causal progression may resume"

    set +e
    wait "$harness_pid"
    local harness_status=$?
    set -e
    ACTIVE_HARNESS_PID=""
    if [[ "$harness_status" != "0" ]]; then
        echo "[FAIL] historical comparator harness failed after replay authority restart"
        cat "$harness_stdout" || true
        return "$harness_status"
    fi

    local log_dir compare_log controller_log
    log_dir="$(awk -F': ' '/^LOGS[[:space:]]*: /{print $2; exit}' "$harness_stdout")"
    [[ -n "$log_dir" && -d "$log_dir" ]] || {
        echo "[FAIL] could not resolve historical run log directory"
        cat "$harness_stdout" || true
        return 1
    }
    compare_log="$log_dir/05_distributed_fast_compare.log"
    controller_log="$log_dir/services/replay-controller.log"
    [[ -f "$compare_log" ]] || { echo "[FAIL] comparator log missing: $compare_log"; return 1; }
    [[ -f "$controller_log" ]] || { echo "[FAIL] replay-controller log missing: $controller_log"; return 1; }

    grep -Fq 'DISTRIBUTED_FAST_COMPARE: PASS' "$compare_log" || {
        echo "[FAIL] authority-restarted distributed path diverged from fast path"
        cat "$compare_log"
        return 1
    }
    grep -Fq 'clock_recovered=true' "$controller_log" || {
        echo "[FAIL] captured final controller log lacks durable clock recovery marker"
        tail -n 220 "$controller_log" || true
        return 1
    }
    if grep -Eq 'reconciliation_blocked|clock_state_rejected|clock.*backwards|same_revision_conflict' "$log_dir"/services/*.log 2>/dev/null; then
        echo "[FAIL] reconciliation/clock safety error observed after authority recovery"
        grep -E 'reconciliation_blocked|clock_state_rejected|clock.*backwards|same_revision_conflict' "$log_dir"/services/*.log | tail -n 100 || true
        return 1
    fi

    echo "[PASS] replay authority restart remained distributed == fast exact"
    echo "[PASS] no rollback, duplicate economic divergence, reconciliation block or clock conflict observed"
    echo "[PASS] STEP 35E authority case: $phase"
    echo "Logs: $log_dir"

    ACTIVE_PROJECT=""
    ACTIVE_BLOCKER=""
    return 0
}

passed=()
for i in "${!REQUESTED[@]}"; do
    phase="${REQUESTED[$i]}"
    if ! run_case "$phase" "$i"; then
        echo
        echo "============================================================"
        echo "STEP 35E CLOCK AUTHORITY RESTART RESULT: FAIL"
        echo "Stopped at phase: $phase"
        echo "============================================================"
        exit 1
    fi
    passed+=("$phase")
done

echo
echo "============================================================"
echo "STEP 35E CLOCK AUTHORITY RESTART RESULT: PASS"
echo "Validated phases: ${passed[*]}"
echo "Proof: replay-controller was hard-killed and deleted while logical time was"
echo "       held at a causal barrier; PostgreSQL retained the exact ClockState while"
echo "       no authority process existed; the replacement installed/re-emitted the"
echo "       same simulation_id/logical_time/revision, did not advance until the"
echo "       blocker was released, and the final replay remained distributed == fast exact."
echo "Next: STEP 35F — replay clock controls + speed/pause economic invariance"
echo "============================================================"
