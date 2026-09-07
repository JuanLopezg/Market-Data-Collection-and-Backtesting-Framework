#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
BASE_COMPOSE="$ROOT/deploy/distributed_replay/docker-compose.yml"
OVERRIDE_COMPOSE="$SCRIPT_DIR/live_clock_isolation.override.yml"
DEPLOY="$ROOT/deploy/distributed_replay"
FORCE_RUNTIME_BUILD="${CLOCK_ISOLATION_FORCE_RUNTIME_BUILD:-1}"
READY_TIMEOUT_SECONDS="${CLOCK_ISOLATION_READY_TIMEOUT_SECONDS:-120}"
LOG_ROOT="$ROOT/validation/logs/clock_live_isolation"
CUSTOM_ISOLATION_DATA="${CLOCK_ISOLATION_HISTORICAL_DATA:-}"
ISOLATION_DATA="${CUSTOM_ISOLATION_DATA:-$LOG_ROOT/isolation_fixture.csv}"
SERVICES=(market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange)

mkdir -p "$LOG_ROOT"

fail() { echo "[FAIL] $*" >&2; exit 1; }

[[ "$FORCE_RUNTIME_BUILD" == "0" || "$FORCE_RUNTIME_BUILD" == "1" ]] \
    || fail "CLOCK_ISOLATION_FORCE_RUNTIME_BUILD must be 0 or 1"
[[ "$READY_TIMEOUT_SECONDS" =~ ^[0-9]+$ ]] && (( READY_TIMEOUT_SECONDS >= 10 )) \
    || fail "CLOCK_ISOLATION_READY_TIMEOUT_SECONDS must be an integer >= 10"
[[ -f "$OVERRIDE_COMPOSE" ]] || fail "missing compose override: $OVERRIDE_COMPOSE"

# This suite proves clock isolation, not historical-data throughput.  Market-data loads its
# entire CSV before event=service_ready, so binding the normal ~5-year source makes this
# gate depend on WSL/Docker bind-mount parsing speed and can create a false readiness
# timeout.  Use a tiny valid OHLCV fixture unless the caller explicitly supplies one.
if [[ -n "$CUSTOM_ISOLATION_DATA" ]]; then
    [[ -f "$CUSTOM_ISOLATION_DATA" ]] \
        || fail "CLOCK_ISOLATION_HISTORICAL_DATA not found: $CUSTOM_ISOLATION_DATA"
else
    cat >"$ISOLATION_DATA" <<'CSV'
date,symbol,open,high,low,close,volume
2021-03-01,BTCUSDT,50000,51000,49000,50500,1000
2021-03-02,BTCUSDT,50500,51500,50000,51000,1100
CSV
fi
[[ -s "$ISOLATION_DATA" ]] || fail "isolation historical fixture is empty: $ISOLATION_DATA"
echo "[PASS] clock-isolation market-data fixture ready: $ISOLATION_DATA"

if [[ "$FORCE_RUNTIME_BUILD" == "1" ]]; then
    ninja -C "$BUILD" -j8 >/dev/null
    bash "$DEPLOY/build_runtime_bundle.sh" >/dev/null
    HISTORICAL_DATA_PATH="$ISOLATION_DATA" docker compose -f "$BASE_COMPOSE" build market-data >/dev/null
    echo "[PASS] current runtime image rebuilt for LIVE/Testnet isolation"
fi

cleanup_project() {
    local project="$1"
    local mode="$2"
    local nats_port="$3"
    local monitor_port="$4"
    local pg_port="$5"
    CLOCK_ISOLATION_RUNTIME_MODE="$mode" \
    HISTORICAL_DATA_PATH="$ISOLATION_DATA" \
    REPLAY_NATS_PORT="$nats_port" \
    REPLAY_NATS_MONITOR_PORT="$monitor_port" \
    REPLAY_POSTGRES_PORT="$pg_port" \
      docker compose -p "$project" -f "$BASE_COMPOSE" -f "$OVERRIDE_COMPOSE" \
        down -v --remove-orphans >/dev/null 2>&1 || true
}

wait_http() {
    local url="$1"
    python3 - "$url" <<'PY'
import sys, time, urllib.request
url=sys.argv[1]
for _ in range(120):
    try:
        with urllib.request.urlopen(url, timeout=1) as r:
            if r.status == 200:
                raise SystemExit(0)
    except Exception:
        time.sleep(0.25)
raise SystemExit(1)
PY
}

check_jsz_isolation() {
    local monitor_port="$1"
    python3 - "$monitor_port" <<'PY'
import json, sys, urllib.request
port=sys.argv[1]
url=f"http://127.0.0.1:{port}/jsz?streams=true&consumers=true&config=true"
with urllib.request.urlopen(url, timeout=5) as r:
    data=json.load(r)
serialized=json.dumps(data, sort_keys=True)
for forbidden in (
    "simulation.clock.state.v1",
    "simulation.clock.sync.request.v1",
    "simulation.clock.control.v1",
    "-clock-state",
):
    if forbidden in serialized:
        print(f"[FAIL] JetStream LIVE/Testnet isolation contains forbidden clock token: {forbidden}", file=sys.stderr)
        raise SystemExit(1)
print("[PASS] JetStream contains no fake-clock subjects or durable clock consumers")
PY
}

run_case() {
    local mode="$1"
    local offset="$2"
    local project="algotrading_clock_isolation_${mode}"
    local nats_port=$((54600 + offset))
    local monitor_port=$((58600 + offset))
    local pg_port=$((55800 + offset))
    local log_dir="$LOG_ROOT/$mode"
    mkdir -p "$log_dir"

    cleanup_project "$project" "$mode" "$nats_port" "$monitor_port" "$pg_port"
    trap 'cleanup_project "$project" "$mode" "$nats_port" "$monitor_port" "$pg_port"' RETURN

    echo
    echo "============================================================"
    echo "STEP 35G LIVE ISOLATION — ${mode^^}"
    echo "============================================================"

    export CLOCK_ISOLATION_RUNTIME_MODE="$mode"
    export HISTORICAL_DATA_PATH="$ISOLATION_DATA"
    export REPLAY_NATS_PORT="$nats_port"
    export REPLAY_NATS_MONITOR_PORT="$monitor_port"
    export REPLAY_POSTGRES_PORT="$pg_port"

    local compose=(docker compose -p "$project" -f "$BASE_COMPOSE" -f "$OVERRIDE_COMPOSE")
    "${compose[@]}" up -d --no-build nats postgres "${SERVICES[@]}" >"$log_dir/compose_up.log" 2>&1 \
        || { cat "$log_dir/compose_up.log"; fail "$mode isolation topology failed to start"; }

    wait_http "http://127.0.0.1:${monitor_port}/varz" \
        || fail "$mode NATS monitor did not become ready"

    local expected_mode="${mode^^}"
    for service in "${SERVICES[@]}"; do
        local cid=""
        local ready=0
        local deadline=$(( $(date +%s) + READY_TIMEOUT_SECONDS ))
        while (( $(date +%s) < deadline )); do
            cid="$("${compose[@]}" ps -q "$service" 2>/dev/null || true)"
            if [[ -n "$cid" ]]; then
                local running_state
                running_state="$(docker inspect -f '{{.State.Running}}' "$cid" 2>/dev/null || true)"
                if [[ "$running_state" == "true" ]]; then
                    if "${compose[@]}" logs --no-color "$service" 2>/dev/null | grep -Fq "event=service_ready"; then
                        ready=1
                        break
                    fi
                else
                    local status
                    status="$(docker inspect -f '{{.State.Status}} exit={{.State.ExitCode}} restart_count={{.RestartCount}}' "$cid" 2>/dev/null || true)"
                    if [[ "$status" == exited* || "$status" == dead* ]]; then
                        break
                    fi
                fi
            fi
            sleep 0.25
        done

        if [[ "$ready" != "1" ]]; then
            "${compose[@]}" logs --no-color "$service" >"$log_dir/${service}.log" 2>&1 || true
            if [[ -n "$cid" ]]; then
                docker inspect "$cid" >"$log_dir/${service}.inspect.json" 2>/dev/null || true
            fi
            echo "[DIAG] $mode $service readiness timeout=${READY_TIMEOUT_SECONDS}s fixture=$ISOLATION_DATA" >&2
            if [[ -n "$cid" ]]; then
                docker inspect -f '[DIAG] status={{.State.Status}} running={{.State.Running}} exit={{.State.ExitCode}} restart_count={{.RestartCount}} started={{.State.StartedAt}}' "$cid" >&2 2>/dev/null || true
            fi
            tail -n 80 "$log_dir/${service}.log" >&2 || true
            [[ -n "$cid" ]] || fail "$mode $service container missing"
            [[ "$(docker inspect -f '{{.State.Running}}' "$cid" 2>/dev/null || true)" == "true" ]] \
                || fail "$mode $service not running"
            fail "$mode $service never became ready"
        fi

        [[ -n "$cid" ]] || fail "$mode $service container missing"
        [[ "$(docker inspect -f '{{.State.Running}}' "$cid")" == "true" ]] || fail "$mode $service not running"

        local cmd
        cmd="$(docker inspect -f '{{json .Config.Cmd}}' "$cid")"
        [[ "$cmd" == *"--runtime-mode"* && "$cmd" == *"$mode"* ]] \
            || fail "$mode $service did not start in requested runtime mode"
        [[ "$cmd" != *"--simulation-id"* ]] \
            || fail "$mode $service unexpectedly received --simulation-id"

        local logs
        logs="$("${compose[@]}" logs --no-color "$service" 2>&1)"
        grep -Fq "event=service_ready" <<<"$logs" || fail "$mode $service never became ready"
        grep -Fq "runtime_mode=$expected_mode" <<<"$logs" \
            || fail "$mode $service did not advertise runtime_mode=$expected_mode"
        if grep -Eq "event=(clock_sync_requested|clock_state_applied|clock_synchronized|clock_bootstrap_ready|clock_gate_waiting)" <<<"$logs"; then
            echo "$logs" >"$log_dir/${service}.log"
            fail "$mode $service activated REPLAY fake-clock control plane"
        fi
        echo "$logs" >"$log_dir/${service}.log"
        echo "[PASS] $service ready on SystemClock path; no simulation id / fake-clock activity"
    done

    check_jsz_isolation "$monitor_port"

    "${compose[@]}" ps -a >"$log_dir/ps.txt"
    echo "[PASS] ${mode^^} runtime booted all seven services with no fake-clock dependency"

    cleanup_project "$project" "$mode" "$nats_port" "$monitor_port" "$pg_port"
    trap - RETURN
}

run_case live 0
run_case testnet 1

echo
echo "============================================================"
echo "STEP 35G LIVE/TESTNET CLOCK ISOLATION RESULT: PASS"
echo "Proof: both production modes started all seven runtime services without"
echo "       --simulation-id, fake-clock subjects, durable clock consumers or"
echo "       REPLAY clock bootstrap activity."
echo "============================================================"
