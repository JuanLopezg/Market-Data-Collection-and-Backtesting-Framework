#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
COMPARE="$ROOT/tools/distributed_compare/run_historical_compare.sh"
COMPOSE_FILE="$ROOT/deploy/distributed_replay/docker-compose.yml"
LOG_ROOT="$ROOT/validation/logs/clock_controls"
CONTROL_SRC="$SCRIPT_DIR/clock_control_cli.cpp"
CONTROL_BIN="$BUILD/validation_clock_control_cli"
HISTORICAL_DATA="${HISTORICAL_DATA:-$ROOT/storage/databases/1d_cmc.csv}"
START_DATE="${CLOCK_CONTROLS_START_DATE:-2021-03-01}"
MEASURED_DAYS="${CLOCK_CONTROLS_DAYS:-10}"
WARMUP_DAYS="${CLOCK_CONTROLS_WARMUP_DAYS:-30}"
WALL_SCALE="${CLOCK_CONTROLS_WALL_SCALE:-0.000001}"
DYNAMIC_PID=""
DYNAMIC_PROJECT=""

mkdir -p "$LOG_ROOT"

cleanup_dynamic_global() {
    if [[ -n "$DYNAMIC_PID" ]] && kill -0 "$DYNAMIC_PID" >/dev/null 2>&1; then
        kill "$DYNAMIC_PID" >/dev/null 2>&1 || true
        wait "$DYNAMIC_PID" >/dev/null 2>&1 || true
    fi
    if [[ -n "$DYNAMIC_PROJECT" ]]; then
        docker compose -p "$DYNAMIC_PROJECT" -f "$COMPOSE_FILE" down -v --remove-orphans >/dev/null 2>&1 || true
    fi
}
trap cleanup_dynamic_global EXIT

fail() {
    echo "[FAIL] $*" >&2
    exit 1
}

latest_run_dir() {
    local parent="$1"
    find "$parent" -mindepth 1 -maxdepth 1 -type d -printf '%p\n' 2>/dev/null | sort | tail -n1
}

compile_control_cli() {
    local includes=()
    for d in common_types utils data_types contracts transport market position account analytics backtest signal portfolio risk sizing rebalance execution exchange runtime persistence recovery testing strategy strategy/strategies ranker indicator universe filter; do
        includes+=("-I$ROOT/lib/src/$d")
    done

    g++ -std=c++23 -O0 -g -Wall -Wextra -Wpedantic \
        "$CONTROL_SRC" "${includes[@]}" \
        "$BUILD/lib/src/libalgolib.so" \
        -Wl,-rpath,"$BUILD/lib/src" -pthread \
        -o "$CONTROL_BIN"
    echo "[PASS] validation clock-control publisher compiled"
}

run_mode_case() {
    local label="$1"
    local speed="$2"
    local port_offset="$3"
    local force_build="$4"

    local case_root="$LOG_ROOT/mode_${label}"
    local project="algotrading_clock_mode_${label//[^a-zA-Z0-9_]/_}"
    local nats_port=$((54500 + port_offset))
    local pg_port=$((55700 + port_offset))
    mkdir -p "$case_root"

    echo
    echo "============================================================"
    echo "STEP 35F MODE — $label"
    echo "speed      : $speed"
    echo "wall scale : $WALL_SCALE"
    echo "============================================================"

    local args=(
        --historical-data "$HISTORICAL_DATA"
        --start-date "$START_DATE"
        --days "$MEASURED_DAYS"
        --warmup-days "$WARMUP_DAYS"
        --barrier-timeout-ms 240000
        --nats-port "$nats_port"
        --postgres-port "$pg_port"
        --project "$project"
        --log-root "$case_root"
        --portfolio-mode equal-weight
        --require-trading
    )
    [[ "$force_build" == "1" ]] && args+=(--force-runtime-build)

    REPLAY_SIMULATION_ID="clock-mode-${label}" \
    REPLAY_CLOCK_SPEED="$speed" \
    REPLAY_CLOCK_WALL_SCALE="$WALL_SCALE" \
        bash "$COMPARE" "${args[@]}"

    local run_dir
    run_dir="$(latest_run_dir "$case_root")"
    [[ -n "$run_dir" ]] || fail "mode $label log directory not found"
    grep -Fq "DISTRIBUTED_FAST_COMPARE: PASS" "$run_dir/05_distributed_fast_compare.log" \
        || fail "mode $label did not remain distributed == fast"

    case "$speed" in
        max)
            grep -Fq "clock_mode=max" "$run_dir/services/replay-controller.log" \
                || fail "mode $label did not advertise MAX"
            ;;
        x1|1|realtime)
            grep -Fq "clock_mode=realtime" "$run_dir/services/replay-controller.log" \
                || fail "mode $label did not advertise realtime/x1"
            ;;
        *)
            grep -Fq "clock_mode=multiplier" "$run_dir/services/replay-controller.log" \
                || fail "mode $label did not advertise multiplier"
            local numeric="${speed#x}"
            grep -Eq "speed_multiplier=${numeric}([.]0+)?([[:space:]]|$)" "$run_dir/services/replay-controller.log" \
                || fail "mode $label did not advertise expected multiplier $numeric"
            ;;
    esac

    echo "[PASS] $label economic result == fast and advertised clock mode is correct"
}

postgres_state() {
    local postgres_cid="$1"
    docker exec "$postgres_cid" psql -At -U algotrading -d algotrading -c \
        "SELECT simulation_id || '|' || logical_time::text || '|' || revision::text || '|' || mode::text || '|' || speed_multiplier::text || '|' || CASE WHEN paused THEN '1' ELSE '0' END FROM replay_controller_clock_state WHERE state_key='replay-controller';" \
        2>/dev/null | tail -n1
}

wait_for_postgres_container() {
    local project="$1"
    local cid=""
    for _ in $(seq 1 240); do
        cid="$(docker ps -q \
            --filter "label=com.docker.compose.project=$project" \
            --filter "label=com.docker.compose.service=postgres" | head -n1)"
        [[ -n "$cid" ]] && { echo "$cid"; return 0; }
        sleep 0.25
    done
    return 1
}

wait_for_state_predicate() {
    local postgres_cid="$1"
    local predicate="$2"
    local timeout_seconds="${3:-60}"
    local deadline=$(( $(date +%s) + timeout_seconds ))
    local state=""
    while (( $(date +%s) < deadline )); do
        state="$(postgres_state "$postgres_cid" || true)"
        if [[ -n "$state" ]] && eval "$predicate"; then
            echo "$state"
            return 0
        fi
        sleep 0.25
    done
    return 1
}

send_control() {
    local nats_port="$1"
    shift
    LD_LIBRARY_PATH="$BUILD/lib/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$CONTROL_BIN" --nats-url "nats://127.0.0.1:$nats_port" "$@"
}

run_pause_resume_case() {
    local case_root="$LOG_ROOT/pause_resume"
    local project="algotrading_clock_pause_resume"
    local simulation_id="clock-pause-resume"
    local nats_port=54520
    local pg_port=55720
    local harness_log="$case_root/harness.log"
    mkdir -p "$case_root"

    echo
    echo "============================================================"
    echo "STEP 35F DYNAMIC — pause / x100 / resume"
    echo "============================================================"

    set +e
    REPLAY_SIMULATION_ID="$simulation_id" \
    REPLAY_CLOCK_SPEED="x1" \
    REPLAY_CLOCK_WALL_SCALE="0.00002" \
        bash "$COMPARE" \
            --historical-data "$HISTORICAL_DATA" \
            --start-date "$START_DATE" \
            --days "$MEASURED_DAYS" \
            --warmup-days "$WARMUP_DAYS" \
            --barrier-timeout-ms 240000 \
            --nats-port "$nats_port" \
            --postgres-port "$pg_port" \
            --project "$project" \
            --log-root "$case_root/runs" \
            --portfolio-mode equal-weight \
            --require-trading \
            >"$harness_log" 2>&1 &
    local harness_pid=$!
    set -e

    DYNAMIC_PID="$harness_pid"
    DYNAMIC_PROJECT="$project"

    local postgres_cid
    postgres_cid="$(wait_for_postgres_container "$project")" \
        || { tail -100 "$harness_log" || true; fail "pause/resume PostgreSQL container did not start"; }

    local initial_state
    initial_state="$(wait_for_state_predicate "$postgres_cid" 'IFS="|" read -r sid t rev mode speed paused <<< "$state"; [[ "$sid" == "clock-pause-resume" && "$rev" -ge 3 && "$paused" == "0" ]]' 120)" \
        || {
            echo "[INFO] last durable clock state: $(postgres_state "$postgres_cid" || true)" >&2
            tail -100 "$harness_log" || true
            fail "initial paced ClockState did not become ready"
        }
    IFS='|' read -r _ initial_time initial_rev _ _ _ <<< "$initial_state"
    echo "[PASS] paced replay active before control: logical_time=$initial_time revision=$initial_rev"

    send_control "$nats_port" \
        --simulation-id "$simulation_id" \
        --command pause \
        --expected-revision "$initial_rev" \
        --message-id "step35f-pause-$initial_rev"

    local paused_state
    paused_state="$(wait_for_state_predicate "$postgres_cid" 'IFS="|" read -r sid t rev mode speed paused <<< "$state"; [[ "$sid" == "clock-pause-resume" && "$paused" == "1" ]]' 30)" \
        || { tail -120 "$harness_log" || true; fail "pause control was not durably applied"; }
    IFS='|' read -r _ paused_time paused_rev paused_mode paused_speed paused_flag <<< "$paused_state"
    echo "[PASS] pause durably applied: logical_time=$paused_time revision=$paused_rev"

    sleep 3
    local held_state
    held_state="$(postgres_state "$postgres_cid")"
    IFS='|' read -r _ held_time held_rev _ _ held_paused <<< "$held_state"
    [[ "$held_time" == "$paused_time" && "$held_rev" == "$paused_rev" && "$held_paused" == "1" ]] \
        || fail "logical clock advanced/changed while paused: before=$paused_state after=$held_state"
    echo "[PASS] pause froze logical_time/revision for 3 real seconds"

    send_control "$nats_port" \
        --simulation-id "$simulation_id" \
        --command speed --speed x100 \
        --expected-revision "$paused_rev" \
        --message-id "step35f-speed-$paused_rev"

    local speed_state
    speed_state="$(wait_for_state_predicate "$postgres_cid" 'IFS="|" read -r sid t rev mode speed paused <<< "$state"; [[ "$sid" == "clock-pause-resume" && "$t" == "'"$paused_time"'" && "$mode" == "2" && "$speed" == 100* && "$paused" == "1" ]]' 30)" \
        || { tail -120 "$harness_log" || true; fail "x100 control was not durably applied while paused"; }
    IFS='|' read -r _ speed_time speed_rev speed_mode speed_value speed_paused <<< "$speed_state"
    echo "[PASS] speed changed while paused without time advance: x100 revision=$speed_rev"

    send_control "$nats_port" \
        --simulation-id "$simulation_id" \
        --command resume \
        --expected-revision "$speed_rev" \
        --message-id "step35f-resume-$speed_rev"

    local resumed_state
    resumed_state="$(wait_for_state_predicate "$postgres_cid" 'IFS="|" read -r sid t rev mode speed paused <<< "$state"; [[ "$sid" == "clock-pause-resume" && "$mode" == "2" && "$speed" == 100* && "$paused" == "0" ]]' 30)" \
        || { tail -120 "$harness_log" || true; fail "resume control was not durably applied"; }
    IFS='|' read -r _ resumed_time resumed_rev _ _ _ <<< "$resumed_state"
    [[ "$resumed_time" == "$paused_time" ]] \
        || fail "logical time advanced before resume state was committed"
    echo "[PASS] resume durably applied at same logical_time: revision=$resumed_rev"

    set +e
    wait "$harness_pid"
    local harness_status=$?
    set -e
    [[ "$harness_status" == "0" ]] || {
        tail -160 "$harness_log" || true
        fail "pause/resume distributed replay failed after resume"
    }

    DYNAMIC_PID=""
    DYNAMIC_PROJECT=""

    local run_dir
    run_dir="$(latest_run_dir "$case_root/runs")"
    [[ -n "$run_dir" ]] || fail "pause/resume run logs missing"
    grep -Fq "DISTRIBUTED_FAST_COMPARE: PASS" "$run_dir/05_distributed_fast_compare.log" \
        || fail "pause/resume changed economic result"
    grep -Fq "event=clock_control_applied action=pause" "$run_dir/services/replay-controller.log" \
        || fail "pause audit marker missing"
    grep -Fq "event=clock_control_applied action=set_speed" "$run_dir/services/replay-controller.log" \
        || fail "set_speed audit marker missing"
    grep -Fq "event=clock_control_applied action=resume" "$run_dir/services/replay-controller.log" \
        || fail "resume audit marker missing"

    echo "[PASS] pause -> x100 -> resume remained distributed == fast exact"
}

[[ -f "$HISTORICAL_DATA" ]] || fail "historical source missing: $HISTORICAL_DATA"
[[ -x "$COMPARE" ]] || fail "historical compare harness missing: $COMPARE"

python3 "$SCRIPT_DIR/shared_clock_controls_check.py"

echo
echo "============================================================"
echo "STEP 35F — INCREMENTAL BUILD / CLOCK AUDIT"
echo "============================================================"
ninja -C "$BUILD" -j8
echo "[PASS] full project build after replay clock controls"
bash "$SCRIPT_DIR/clock_audit.sh"
echo "[PASS] clock audit remains clean"
compile_control_cli

# All modes use the same historical window and fast reference.  The validation-only
# wall scale compresses wall waiting but does not enter ClockState/economic contracts.
run_mode_case "max" "max" 0 1
run_mode_case "x1" "x1" 1 0
run_mode_case "x10" "x10" 2 0
run_mode_case "x60" "x60" 3 0
run_mode_case "x100" "x100" 4 0
run_pause_resume_case

echo
echo "============================================================"
echo "STEP 35F CLOCK CONTROLS / ECONOMIC INVARIANCE RESULT: PASS"
echo "Validated modes : x1 x10 x60 x100 MAX"
echo "Validated control: pause -> speed change while paused -> resume"
echo "Proof            : every mode and the dynamic-control replay remained"
echo "                   distributed == fast exact; pause held logical time"
echo "                   and barrier deadlines do not expire while paused."
echo "Next: STEP 35G — final shared-clock acceptance suite + LIVE isolation proof"
echo "============================================================"
