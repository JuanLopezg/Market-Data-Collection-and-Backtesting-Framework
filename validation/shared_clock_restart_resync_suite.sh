#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEPLOY="$ROOT/deploy/distributed_replay"
COMPOSE_FILE="$DEPLOY/docker-compose.yml"
HARNESS="$ROOT/tools/distributed_compare/run_historical_compare.sh"
SOURCE_GATE="$SCRIPT_DIR/shared_clock_restart_resync_check.py"
CLOCK_AUDIT="$SCRIPT_DIR/clock_audit.sh"
HISTORICAL_DATA="${HISTORICAL_DATA_PATH:-$ROOT/storage/databases/1d_cmc.csv}"
LOG_ROOT="$ROOT/validation/logs/clock_restart_resync"
KILL_AFTER_CYCLES="${CLOCK_RESYNC_KILL_AFTER_CYCLES:-20}"
MEASURED_DAYS="${CLOCK_RESYNC_MEASURED_DAYS:-20}"
WARMUP_DAYS="${CLOCK_RESYNC_WARMUP_DAYS:-30}"
BARRIER_TIMEOUT_MS="${CLOCK_RESYNC_BARRIER_TIMEOUT_MS:-240000}"
FORCE_RUNTIME_BUILD="${CLOCK_RESYNC_FORCE_RUNTIME_BUILD:-1}"

ALL_FOLLOWERS=(
    market-data
    strategy
    portfolio-risk
    order-planner
    execution-state
    exchange-gateway
    simulated-exchange
)

ACTIVE_HARNESS_PID=""
ACTIVE_PROJECT=""
ACTIVE_BLOCKER=""

usage() {
    cat <<'EOF'
Usage: bash validation/shared_clock_restart_resync_suite.sh [service ...]

Without arguments the suite validates all seven REPLAY clock followers. To re-run a
single failed case, pass one or more service names:

  market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange

Optional environment:
  CLOCK_RESYNC_KILL_AFTER_CYCLES   default 20
  CLOCK_RESYNC_MEASURED_DAYS       default 20
  CLOCK_RESYNC_WARMUP_DAYS         default 30
  CLOCK_RESYNC_BARRIER_TIMEOUT_MS  default 240000
  CLOCK_RESYNC_FORCE_RUNTIME_BUILD default 1 (force first-case image rebuild)
EOF
}

contains_follower() {
    local candidate="$1" service
    for service in "${ALL_FOLLOWERS[@]}"; do
        [[ "$candidate" == "$service" ]] && return 0
    done
    return 1
}

cleanup_active() {
    local code=$?
    if [[ -n "$ACTIVE_HARNESS_PID" ]] && kill -0 "$ACTIVE_HARNESS_PID" 2>/dev/null; then
        # Preserve the topology on failure through the child harness, but do not leave
        # a live background validation process behind the shell that invoked this suite.
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
[[ -x "$SOURCE_GATE" ]] || { echo "[FAIL] STEP 35D source gate missing: $SOURCE_GATE" >&2; exit 1; }
[[ -x "$CLOCK_AUDIT" ]] || { echo "[FAIL] clock audit missing: $CLOCK_AUDIT" >&2; exit 1; }
[[ -f "$HISTORICAL_DATA" ]] || { echo "[FAIL] historical data missing: $HISTORICAL_DATA" >&2; exit 1; }
[[ "$KILL_AFTER_CYCLES" =~ ^[0-9]+$ ]] || { echo "[FAIL] CLOCK_RESYNC_KILL_AFTER_CYCLES must be an integer" >&2; exit 2; }
[[ "$MEASURED_DAYS" =~ ^[0-9]+$ ]] || { echo "[FAIL] CLOCK_RESYNC_MEASURED_DAYS must be an integer" >&2; exit 2; }
[[ "$WARMUP_DAYS" =~ ^[0-9]+$ ]] || { echo "[FAIL] CLOCK_RESYNC_WARMUP_DAYS must be an integer" >&2; exit 2; }
[[ "$FORCE_RUNTIME_BUILD" == "0" || "$FORCE_RUNTIME_BUILD" == "1" ]] || { echo "[FAIL] CLOCK_RESYNC_FORCE_RUNTIME_BUILD must be 0 or 1" >&2; exit 2; }
(( KILL_AFTER_CYCLES >= 5 )) || { echo "[FAIL] crash threshold must be >= 5 cycles" >&2; exit 2; }
(( MEASURED_DAYS >= 5 && WARMUP_DAYS >= 5 )) || { echo "[FAIL] measured/warmup windows are too small" >&2; exit 2; }
(( KILL_AFTER_CYCLES < MEASURED_DAYS + WARMUP_DAYS - 3 )) || {
    echo "[FAIL] crash threshold leaves too few cycles for post-restart proof" >&2
    exit 2
}

if (( $# == 0 )); then
    REQUESTED=("${ALL_FOLLOWERS[@]}")
else
    if [[ "$1" == "--help" || "$1" == "-h" ]]; then usage; exit 0; fi
    REQUESTED=("$@")
fi
for service in "${REQUESTED[@]}"; do
    contains_follower "$service" || { echo "[FAIL] unsupported follower: $service" >&2; usage >&2; exit 2; }
done

mkdir -p "$LOG_ROOT"

echo "============================================================"
echo "STEP 35D — HARD-RESTART FOLLOWER RE-SYNC / CAUSAL RECOVERY"
echo "============================================================"
python3 "$SOURCE_GATE" --root "$ROOT"
echo
bash "$CLOCK_AUDIT"
echo "[PASS] source architecture + clock audit prerequisites remain clean"

echo "============================================================"
echo "STEP 35D — INCREMENTAL BUILD"
echo "============================================================"
ninja -C "$ROOT/build" -j8
echo "[PASS] full project build before restart injection"
echo "[INFO] first-case runtime image rebuild: $([[ "$FORCE_RUNTIME_BUILD" == "1" ]] && echo FORCED || echo REUSE_ALLOWED)"

pg_scalar() {
    local project="$1" sql="$2"
    docker compose -p "$project" -f "$COMPOSE_FILE" exec -T postgres \
        psql -U algotrading -d algotrading -Atqc "$sql" 2>/dev/null | tr -d '\r'
}

clock_state_row() {
    local project="$1"
    pg_scalar "$project" \
        "SELECT simulation_id || '|' || logical_time::text || '|' || revision::text || '|' || paused::text FROM replay_controller_clock_state WHERE state_key='replay-controller';"
}

completed_cycles() {
    local project="$1"
    docker compose -p "$project" -f "$COMPOSE_FILE" logs --no-color replay-controller 2>/dev/null \
        | grep -c '\[REPLAY\] execution-complete' || true
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

wait_for_stable_clock_barrier() {
    local project="$1" harness_pid="$2"
    local last_state="" last_completed="" stable=0 state cycles
    # Require 8 identical 250 ms observations (~2 s). Because a trading-path blocker is
    # paused while replay-controller stays alive, this demonstrates an active causal
    # barrier with a stable authoritative clock rather than merely sampling a fast tick.
    for _ in $(seq 1 240); do
        kill -0 "$harness_pid" 2>/dev/null || return 1
        state="$(clock_state_row "$project" || true)"
        cycles="$(completed_cycles "$project")"
        if [[ -n "$state" && "$state" == "$last_state" && "$cycles" == "$last_completed" ]]; then
            stable=$((stable + 1))
        else
            stable=0
            last_state="$state"
            last_completed="$cycles"
        fi
        if (( stable >= 8 )); then
            printf '%s\n' "$state"
            return 0
        fi
        sleep 0.25
    done
    return 1
}

extract_sync_field() {
    local line="$1" key="$2"
    sed -n "s/.*${key}=\\([^ ]*\\).*/\\1/p" <<<"$line"
}


authority_refresh_count() {
    local project="$1" simulation_id="$2" logical_time="$3" revision="$4"
    docker compose -p "$project" -f "$COMPOSE_FILE" logs --no-color replay-controller 2>/dev/null \
        | grep -F "event=clock_state_refresh simulation_id=$simulation_id logical_time=$logical_time revision=$revision" \
        | wc -l | tr -d ' ' || true
}

run_case() {
    local target="$1" index="$2"
    local safe="${target//-/_}"
    local project="algotrading_clock_resync_${safe}"
    local nats_port=$((54310 + index))
    local monitor_port=$((58310 + index))
    local postgres_port=$((55510 + index))
    local simulation_id="clock-resync-${safe}"
    local blocker="order-planner"
    [[ "$target" == "order-planner" ]] && blocker="strategy"

    local stamp
    stamp="$(date +%Y%m%d_%H%M%S)"
    local case_root="$LOG_ROOT/$safe"
    local harness_stdout="$case_root/${stamp}_harness.log"
    local recreated_log="$case_root/${stamp}_recreated_${safe}.log"
    local authority_hold_log="$case_root/${stamp}_authority_hold.log"
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

    # Remove leftovers from an earlier interrupted attempt of this exact case.
    "${compose[@]}" down -v --remove-orphans >/dev/null 2>&1 || true

    echo
    echo "============================================================"
    echo "STEP 35D CASE — $target hard restart / exact clock re-sync"
    echo "Project       : $project"
    echo "Simulation ID : $simulation_id"
    echo "Blocker       : $blocker (paused while authority still polls sync requests)"
    echo "Window        : 2021-03-01 + ${MEASURED_DAYS} measured, ${WARMUP_DAYS} warmup"
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

    local target_id blocker_id controller_id
    target_id="$(wait_for_container "$project" "$target" "$harness_pid")" || {
        echo "[FAIL] target container did not become available: $target"
        cat "$harness_stdout" || true
        return 1
    }
    blocker_id="$(wait_for_container "$project" "$blocker" "$harness_pid")" || {
        echo "[FAIL] blocker container did not become available: $blocker"
        return 1
    }
    controller_id="$(wait_for_container "$project" replay-controller "$harness_pid")" || {
        echo "[FAIL] replay-controller container did not become available"
        return 1
    }
    echo "[PASS] runtime containers available: target=$target blocker=$blocker controller=$controller_id"

    local completed=0
    for _ in $(seq 1 2400); do
        if ! kill -0 "$harness_pid" 2>/dev/null; then
            echo "[FAIL] harness exited before restart threshold"
            cat "$harness_stdout" || true
            return 1
        fi
        completed="$(completed_cycles "$project")"
        (( completed >= KILL_AFTER_CYCLES )) && break
        sleep 0.25
    done
    (( completed >= KILL_AFTER_CYCLES )) || { echo "[FAIL] replay did not reach restart threshold"; return 1; }
    echo "[PASS] replay reached restart threshold: completed_cycles=$completed"

    # Freeze normal economic progression without freezing replay-controller. This is what
    # makes the re-sync proof strong: the authority can answer the startup request, but it
    # cannot naturally advance to a new clock tick while the target is being recreated.
    docker pause "$blocker_id" >/dev/null
    ACTIVE_BLOCKER="$blocker"
    echo "[INFO] paused blocker $blocker; waiting for a stable authority barrier"

    local held_state
    held_state="$(wait_for_stable_clock_barrier "$project" "$harness_pid")" || {
        echo "[FAIL] authoritative clock did not become stable behind blocker=$blocker"
        "${compose[@]}" logs --tail=160 replay-controller 2>/dev/null || true
        return 1
    }

    local held_sim held_time held_revision held_paused
    IFS='|' read -r held_sim held_time held_revision held_paused <<<"$held_state"
    [[ "$held_sim" == "$simulation_id" && "$held_time" =~ ^[0-9]+$ && "$held_revision" =~ ^[0-9]+$ ]] || {
        echo "[FAIL] malformed/unexpected durable authority state: $held_state"
        return 1
    }
    [[ "$held_paused" == "f" || "$held_paused" == "false" ]] || {
        echo "[FAIL] STEP 35D expected trading clock running while business blocker is paused: $held_state"
        return 1
    }
    echo "[PASS] authority held stable: simulation_id=$held_sim logical_time=$held_time revision=$held_revision"

    # Compose uses restart:on-failure. A raw SIGKILL can race Docker's automatic
    # restart manager with the suite's subsequent docker rm -f, creating a short-lived
    # intermediate process. That process can issue/consume clock re-sync traffic and make
    # controller logs look successful even though the final recreated container never
    # participated. Disable the victim's restart policy before the injected SIGKILL.
    local original_restart_policy original_restart_count disabled_restart_policy
    original_restart_policy="$(docker inspect -f '{{.HostConfig.RestartPolicy.Name}}' "$target_id" 2>/dev/null || true)"
    original_restart_count="$(docker inspect -f '{{.RestartCount}}' "$target_id" 2>/dev/null || true)"
    [[ "$original_restart_count" =~ ^[0-9]+$ ]] || original_restart_count=0

    echo "[INFO] isolating SIGKILL from Docker restart policy: current=${original_restart_policy:-<unknown>} -> no"
    docker update --restart=no "$target_id" >/dev/null
    disabled_restart_policy="$(docker inspect -f '{{.HostConfig.RestartPolicy.Name}}' "$target_id" 2>/dev/null || true)"
    [[ "$disabled_restart_policy" == "no" ]] || {
        echo "[FAIL] could not disable restart policy before hard-kill injection"
        echo "policy=$disabled_restart_policy"
        return 1
    }

    echo "[INFO] hard-killing + deleting follower container: $target"
    docker kill --signal KILL "$target_id" >/dev/null 2>&1 || true

    local old_running="true"
    for _ in $(seq 1 80); do
        old_running="$(docker inspect -f '{{.State.Running}}' "$target_id" 2>/dev/null || echo false)"
        [[ "$old_running" == "false" ]] && break
        sleep 0.10
    done
    [[ "$old_running" == "false" ]] || {
        echo "[FAIL] hard-killed $target did not reach stopped state"
        return 1
    }

    local after_kill_restart_count
    after_kill_restart_count="$(docker inspect -f '{{.RestartCount}}' "$target_id" 2>/dev/null || true)"
    [[ "$after_kill_restart_count" =~ ^[0-9]+$ ]] || after_kill_restart_count=0
    [[ "$after_kill_restart_count" == "$original_restart_count" ]] || {
        echo "[FAIL] Docker auto-restarted the victim during the hard-kill proof"
        echo "restart_count_before=$original_restart_count restart_count_after=$after_kill_restart_count"
        return 1
    }
    echo "[PASS] SIGKILL produced exactly one stopped process; no automatic intermediate restart"

    docker rm -f "$target_id" >/dev/null 2>&1 || true

    # Baseline only after the victim is fully removed. Any later same-revision refresh
    # is eligible to reach the final replacement, never a transient auto-restart.
    local refresh_before
    refresh_before="$(authority_refresh_count "$project" "$held_sim" "$held_time" "$held_revision")"
    [[ "$refresh_before" =~ ^[0-9]+$ ]] || refresh_before=0

    "${compose[@]}" up -d --no-deps --no-build "$target" >/dev/null

    local new_id
    new_id="$("${compose[@]}" ps -q "$target" 2>/dev/null || true)"
    [[ -n "$new_id" && "$new_id" != "$target_id" ]] || {
        echo "[FAIL] $target was not recreated with a new container id"
        return 1
    }

    local new_running="false"
    for _ in $(seq 1 160); do
        new_running="$(docker inspect -f '{{.State.Running}}' "$new_id" 2>/dev/null || true)"
        [[ "$new_running" == "true" ]] && break
        sleep 0.25
    done
    [[ "$new_running" == "true" ]] || {
        echo "[FAIL] recreated $target container never reached running state"
        docker inspect "$new_id" 2>/dev/null || true
        docker logs --tail=200 "$new_id" 2>&1 || true
        return 1
    }

    local recreated_restart_policy
    recreated_restart_policy="$(docker inspect -f '{{.HostConfig.RestartPolicy.Name}}' "$new_id" 2>/dev/null || true)"
    [[ "$recreated_restart_policy" == "on-failure" ]] || {
        echo "[FAIL] recreated $target did not restore Compose restart:on-failure policy"
        echo "policy=$recreated_restart_policy"
        return 1
    }
    echo "[PASS] final replacement restored restart policy: on-failure"

    local recreated_cmd
    recreated_cmd="$(docker inspect -f '{{json .Config.Cmd}}' "$new_id" 2>/dev/null || true)"
    grep -Fq -- '--runtime-mode' <<<"$recreated_cmd" && grep -Fq 'replay' <<<"$recreated_cmd" || {
        echo "[FAIL] recreated $target did not boot with --runtime-mode replay"
        echo "command: $recreated_cmd"
        return 1
    }
    echo "[PASS] follower recreated/running in REPLAY: $target $target_id -> $new_id"

    local sync_line="" bootstrap_line="" request_line="" request_message_id=""
    local requested_seen=0 request_attempt_seen=0 response_seen=0 refresh_seen=0
    local refresh_after="$refresh_before"
    for _ in $(seq 1 600); do
        kill -0 "$harness_pid" 2>/dev/null || break
        local target_logs controller_logs
        target_logs="$(docker logs "$new_id" 2>&1 || true)"
        controller_logs="$("${compose[@]}" logs --no-color replay-controller 2>/dev/null || true)"
        grep -Fq 'event=clock_sync_request_attempt' <<<"$target_logs" && request_attempt_seen=1
        request_line="$(grep -F 'event=clock_sync_requested' <<<"$target_logs" | tail -n 1 || true)"
        if [[ -n "$request_line" ]]; then
            requested_seen=1
            request_message_id="$(extract_sync_field "$request_line" message_id)"
        fi
        sync_line="$(grep -F 'event=clock_synchronized' <<<"$target_logs" | tail -n 1 || true)"
        bootstrap_line="$(grep -F 'event=clock_bootstrap_ready' <<<"$target_logs" | tail -n 1 || true)"

        # Accept a sync response only if the controller correlation id is the message id
        # emitted by THIS final replacement container. This removes false positives from
        # any process that existed before the replacement container id was created.
        if [[ -n "$request_message_id" ]] && \
           grep -Fq "event=clock_sync_response requester=$target simulation_id=$held_sim logical_time=$held_time revision=$held_revision" <<<"$controller_logs" && \
           grep -Fq "correlation_id=$request_message_id" <<<"$controller_logs"; then
            response_seen=1
        fi

        refresh_after="$(authority_refresh_count "$project" "$held_sim" "$held_time" "$held_revision")"
        [[ "$refresh_after" =~ ^[0-9]+$ ]] || refresh_after=0
        if (( refresh_after > refresh_before )); then
            refresh_seen=1
        fi
        if (( response_seen == 1 || refresh_seen == 1 )) && [[ -n "$bootstrap_line" ]]; then
            break
        fi
        sleep 0.25
    done

    # Two independent transport paths can repair a hard-restarted follower without
    # changing economic time: explicit request/response and periodic same-revision
    # authority refresh. The application proof is clock_bootstrap_ready, emitted only
    # after this process has polled and installed a fresh authority-confirmed snapshot.
    if (( response_seen == 1 )); then
        echo "[PASS] authority answered the FINAL $target replacement with the exact held clock revision"
        echo "[PASS] response correlation matched replacement request: $request_message_id"
    elif (( refresh_seen == 1 )); then
        echo "[PASS] authority emitted a fresh same-revision clock refresh after $target restart"
    else
        echo "[FAIL] no post-restart authoritative ClockState was supplied at the held revision"
        echo "[INFO] request attempt marker=$request_attempt_seen request success marker=$requested_seen request_message_id=${request_message_id:-<none>} refresh_before=$refresh_before refresh_after=$refresh_after"
        docker inspect -f 'status={{.State.Status}} running={{.State.Running}} restart_count={{.RestartCount}} exit={{.State.ExitCode}}' "$new_id" 2>/dev/null || true
        docker logs --tail=220 "$new_id" 2>&1 || true
        "${compose[@]}" logs --tail=260 replay-controller 2>/dev/null || true
        tail -120 "$harness_stdout" 2>/dev/null || true
        return 1
    fi
    [[ -n "$bootstrap_line" ]] || {
        echo "[FAIL] recreated $target never completed authoritative clock bootstrap for the held state"
        echo "[INFO] request attempt marker=$request_attempt_seen request success marker=$requested_seen request_message_id=${request_message_id:-<none>} response_seen=$response_seen refresh_seen=$refresh_seen"
        docker inspect -f 'status={{.State.Status}} running={{.State.Running}} restart_count={{.RestartCount}} exit={{.State.ExitCode}}' "$new_id" 2>/dev/null || true
        docker logs --tail=220 "$new_id" 2>&1 || true
        return 1
    }
    if (( request_attempt_seen == 1 )); then
        echo "[PASS] recreated $target reached clock sync request attempt before business bootstrap"
    else
        echo "[WARN] request-attempt stdout marker not observed live; exact authority snapshot + follower install remain the recovery proof"
    fi
    if (( requested_seen == 1 )); then
        echo "[PASS] recreated $target emitted clock_sync_requested audit marker"
    elif (( refresh_seen == 1 )); then
        echo "[INFO] explicit request success marker not observed; authority refresh fallback recovered the follower"
    fi

    local sync_sim sync_time sync_revision sync_paused
    sync_sim="$(extract_sync_field "$bootstrap_line" simulation_id)"
    sync_time="$(extract_sync_field "$bootstrap_line" logical_time)"
    sync_revision="$(extract_sync_field "$bootstrap_line" revision)"
    sync_paused="$(extract_sync_field "$bootstrap_line" paused)"
    [[ "$sync_sim" == "$held_sim" && "$sync_time" == "$held_time" && "$sync_revision" == "$held_revision" ]] || {
        echo "[FAIL] recreated follower did not install the exact held ClockState"
        echo "held : simulation_id=$held_sim logical_time=$held_time revision=$held_revision"
        echo "sync : simulation_id=$sync_sim logical_time=$sync_time revision=$sync_revision paused=$sync_paused"
        echo "line : $bootstrap_line"
        return 1
    }
    echo "[PASS] exact restart re-sync/bootstrap: simulation_id=$sync_sim logical_time=$sync_time revision=$sync_revision"

    # The business blocker must still hold the authority at the exact same durable state.
    local after_sync_state
    after_sync_state="$(clock_state_row "$project" || true)"
    [[ "$after_sync_state" == "$held_state" ]] || {
        echo "[FAIL] authority advanced while the restart sync proof was supposed to be held"
        echo "held      : $held_state"
        echo "after sync: $after_sync_state"
        return 1
    }
    echo "[PASS] restart recovery installed the exact same revision; authority did not advance"

    docker logs "$new_id" >"$recreated_log" 2>&1 || true
    "${compose[@]}" logs --no-color replay-controller >"$authority_hold_log" 2>&1 || true

    # No monotonicity/simulation-identity poison condition is allowed during recovery.
    if grep -Eq 'event=clock_state_(rejected|invalid|apply_failed)' "$recreated_log"; then
        echo "[FAIL] recreated $target logged a clock-state rejection/apply failure"
        grep -E 'event=clock_state_(rejected|invalid|apply_failed)' "$recreated_log" || true
        return 1
    fi

    docker unpause "$blocker_id" >/dev/null
    ACTIVE_BLOCKER=""
    echo "[INFO] unpaused blocker $blocker; normal causal progression may resume"

    local target_completed=$((completed + 3)) after="$completed"
    local total_cycles=$((MEASURED_DAYS + WARMUP_DAYS))
    (( target_completed >= total_cycles )) && target_completed=$((total_cycles - 1))
    for _ in $(seq 1 1200); do
        if ! kill -0 "$harness_pid" 2>/dev/null; then break; fi
        after="$(completed_cycles "$project")"
        (( after >= target_completed )) && break
        sleep 0.25
    done
    (( after >= target_completed )) || {
        echo "[FAIL] replay did not advance after $target re-sync: before=$completed after=$after"
        docker logs --tail=200 "$new_id" 2>&1 || true
        return 1
    }
    echo "[PASS] replay advanced after exact clock re-sync: cycles=$completed -> $after"

    set +e
    wait "$harness_pid"
    local harness_status=$?
    set -e
    ACTIVE_HARNESS_PID=""
    ACTIVE_PROJECT=""
    if [[ "$harness_status" != "0" ]]; then
        echo "[FAIL] historical comparator harness failed after $target restart"
        cat "$harness_stdout" || true
        return "$harness_status"
    fi

    local run_log_dir
    run_log_dir="$(awk -F': ' '/^LOGS[[:space:]]*: /{print $2; exit}' "$harness_stdout")"
    [[ -n "$run_log_dir" && -d "$run_log_dir" ]] || {
        echo "[FAIL] could not resolve comparator log directory for $target"
        return 1
    }
    local compare_log="$run_log_dir/05_distributed_fast_compare.log"
    local execution_log="$run_log_dir/services/execution-state.log"
    local service_log="$run_log_dir/services/$target.log"
    [[ -f "$compare_log" ]] || { echo "[FAIL] comparator log missing: $compare_log"; return 1; }
    grep -Fq 'DISTRIBUTED_FAST_COMPARE: PASS' "$compare_log" || {
        echo "[FAIL] $target restart changed economic result"
        cat "$compare_log" || true
        return 1
    }
    if [[ -f "$execution_log" ]] && grep -Fq 'event=reconciliation_blocked' "$execution_log"; then
        echo "[FAIL] reconciliation blocked after $target clock recovery"
        grep -F 'event=reconciliation_blocked' "$execution_log" || true
        return 1
    fi
    [[ -f "$service_log" ]] || { echo "[FAIL] final captured follower log missing: $service_log"; return 1; }
    if grep -Eq 'event=clock_state_(rejected|invalid|apply_failed)' "$service_log"; then
        echo "[FAIL] clock monotonicity/identity error observed in final $target log"
        grep -E 'event=clock_state_(rejected|invalid|apply_failed)' "$service_log" || true
        return 1
    fi

    echo "[PASS] $target hard restart remained distributed == fast exact"
    echo "[PASS] no reconciliation block or clock identity/monotonicity error observed"
    echo "[PASS] STEP 35D follower case: $target"
    echo "Logs: $run_log_dir"
}

index=0
for service in "${REQUESTED[@]}"; do
    if ! run_case "$service" "$index"; then
        echo
        echo "============================================================"
        echo "STEP 35D CLOCK RESTART/RESYNC RESULT: FAIL"
        echo "Stopped at follower: $service"
        echo "============================================================"
        exit 1
    fi
    index=$((index + 1))
done

echo
echo "============================================================"
echo "STEP 35D CLOCK RESTART/RESYNC RESULT: PASS"
echo "Validated followers: ${REQUESTED[*]}"
echo "Proof: each recreated follower received a fresh post-restart authority snapshot"
echo "       of the exact held simulation_id/logical_time/revision (request-response or"
echo "       same-revision refresh fallback), resumed causal processing, and remained"
echo "       distributed == fast exact."
echo "Next: STEP 35E — replay-authority hard restart + durable clock recovery proof"
echo "============================================================"
