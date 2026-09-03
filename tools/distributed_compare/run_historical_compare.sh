#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
DEPLOY="$ROOT/deploy/distributed_replay"
COMPARE_SRC="$SCRIPT_DIR/distributed_fast_compare.cpp"
WINDOW_TOOL="$SCRIPT_DIR/historical_window.py"

historical_data="$ROOT/storage/databases/1d_cmc.csv"
start_date=""
days=5
warmup_days=30
initial_cash=100000
commission_rate=0
barrier_timeout_ms=120000
nats_port=54230
nats_monitor_port=""
postgres_port=55440
project="algotrading_historical_compare"
log_root="$ROOT/historical_replay_logs"
require_trading=0
force_runtime_build=0
keep_on_failure="${HISTORICAL_KEEP_ON_FAILURE:-0}"
runtime_image="algotrading-runtime:step28"
portfolio_mode="equal-weight"
realtest_csv=""
realtest_comparison_csv=""

usage() {
    cat <<'EOF'
Usage: tools/distributed_compare/run_historical_compare.sh [options]

Options:
  --historical-data PATH   CSV source (default storage/databases/1d_cmc.csv)
  --start-date YYYY-MM-DD  First measured close. Default: latest feasible window.
  --days N                 Measured days (default 5)
  --warmup-days N          Warmup closes before measured range (default 30)
  --initial-cash X         Default 100000
  --commission-rate X      Default 0
  --barrier-timeout-ms N   Default 120000
  --nats-port N            Host NATS client port (default 54230)
  --nats-monitor-port N    Host NATS monitoring port (default NATS port + 4000)
  --postgres-port N        Host PostgreSQL port (default 55440)
  --project NAME           Docker Compose project name
  --log-root PATH          Parent directory for timestamped logs
  --require-trading        Fail if no orders/fills/closed trades occur
  --force-runtime-build    Rebuild the local runtime image even if it already exists
  --portfolio-mode MODE    equal-weight (default) or vol-target
  --realtest-csv PATH       After fast==distributed, run the exact research RealTest comparator on distributed trades
  --realtest-comparison-csv PATH  Output path used by the research comparator (mainly VolTarget campaign CSV)
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --historical-data) historical_data="$2"; shift 2 ;;
        --start-date) start_date="$2"; shift 2 ;;
        --days) days="$2"; shift 2 ;;
        --warmup-days) warmup_days="$2"; shift 2 ;;
        --initial-cash) initial_cash="$2"; shift 2 ;;
        --commission-rate) commission_rate="$2"; shift 2 ;;
        --barrier-timeout-ms) barrier_timeout_ms="$2"; shift 2 ;;
        --nats-port) nats_port="$2"; shift 2 ;;
        --nats-monitor-port) nats_monitor_port="$2"; shift 2 ;;
        --postgres-port) postgres_port="$2"; shift 2 ;;
        --project) project="$2"; shift 2 ;;
        --log-root) log_root="$2"; shift 2 ;;
        --require-trading) require_trading=1; shift ;;
        --force-runtime-build) force_runtime_build=1; shift ;;
        --portfolio-mode) portfolio_mode="$2"; shift 2 ;;
        --realtest-csv) realtest_csv="$2"; shift 2 ;;
        --realtest-comparison-csv) realtest_comparison_csv="$2"; shift 2 ;;
        --help|-h) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
done

if [[ -z "$nats_monitor_port" ]]; then
    nats_monitor_port=$((nats_port + 4000))
fi

[[ "$nats_port" =~ ^[0-9]+$ ]] && (( nats_port >= 1 && nats_port <= 65535 )) || { echo "[FAIL] invalid --nats-port: $nats_port" >&2; exit 2; }
[[ "$nats_monitor_port" =~ ^[0-9]+$ ]] && (( nats_monitor_port >= 1 && nats_monitor_port <= 65535 )) || { echo "[FAIL] invalid --nats-monitor-port: $nats_monitor_port" >&2; exit 2; }
[[ "$postgres_port" =~ ^[0-9]+$ ]] && (( postgres_port >= 1 && postgres_port <= 65535 )) || { echo "[FAIL] invalid --postgres-port: $postgres_port" >&2; exit 2; }

case "$portfolio_mode" in
    equal-weight) portfolio_config="/opt/algotrading/config/portfolio/pure_rsi_equal_weight.json" ;;
    vol-target) portfolio_config="/opt/algotrading/config/portfolio/pure_rsi_vol_target.json" ;;
    *) echo "[FAIL] invalid --portfolio-mode: $portfolio_mode (expected equal-weight or vol-target)" >&2; exit 2 ;;
esac

[[ -f "$historical_data" ]] || {
    echo "[FAIL] historical source not found: $historical_data" >&2
    echo "       Pass --historical-data /path/to/1d_cmc.csv" >&2
    exit 1
}
[[ -f "$COMPARE_SRC" ]] || { echo "[FAIL] comparator missing: $COMPARE_SRC" >&2; exit 1; }
if [[ -n "$realtest_csv" ]]; then
    [[ -f "$realtest_csv" ]] || { echo "[FAIL] RealTest CSV not found: $realtest_csv" >&2; exit 1; }
fi
[[ -x "$WINDOW_TOOL" ]] || { echo "[FAIL] window resolver missing: $WINDOW_TOOL" >&2; exit 1; }

window_args=(--historical-data "$historical_data" --days "$days" --warmup-days "$warmup_days")
[[ -n "$start_date" ]] && window_args+=(--start-date "$start_date")
window_output="$(python3 "$WINDOW_TOOL" "${window_args[@]}")" || {
    echo "$window_output" >&2
    exit 1
}
eval "$window_output"

STAMP="$(date +%Y%m%d_%H%M%S)"
LOG_DIR="$log_root/$STAMP"
WORK_DIR="$LOG_DIR/work"
mkdir -p "$LOG_DIR" "$WORK_DIR"
COMPOSE=(docker compose -p "$project" -f "$DEPLOY/docker-compose.yml")

cleanup() {
    local exit_code=$?
    capture_service_logs || true
    if [[ "$keep_on_failure" == "1" && "$exit_code" != "0" ]]; then
        echo "[INFO] preserving failed Compose topology for diagnostics: project=$project nats_port=$nats_port nats_monitor_port=$nats_monitor_port"
        echo "[INFO] cleanup when finished: docker compose -p $project -f $DEPLOY/docker-compose.yml down -v --remove-orphans"
        return
    fi
    "${COMPOSE[@]}" down -v --remove-orphans >/dev/null 2>&1 || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM


capture_service_logs() {
    mkdir -p "$LOG_DIR/services"
    for service in replay-controller market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange; do
        "${COMPOSE[@]}" logs --no-color "$service" >"$LOG_DIR/services/${service}.log" 2>&1 || true
    done
}

dump_logs() {
    echo "---- compose ps -a ----"
    "${COMPOSE[@]}" ps -a || true
    for service in replay-controller market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange; do
        echo "---- $service ----"
        "${COMPOSE[@]}" logs --tail=80 "$service" 2>/dev/null || true
    done
}

compile_comparator() {
    local includes=()
    for d in common_types utils data_types contracts transport market position account analytics backtest signal portfolio risk sizing rebalance execution exchange runtime persistence recovery testing strategy strategy/strategies ranker indicator universe filter; do
        includes+=("-I$ROOT/lib/src/$d")
    done
    local pkg_cflags=() pkg_libs=()
    # The comparator now compiles research/src/realtest.cpp directly so it must
    # receive the same direct fmt/log4cpp dependencies that Meson supplies to
    # algotrading_research via libalgolib_dep.  libpq/libnats were already here.
    read -r -a pkg_cflags <<< "$(pkg-config --cflags libpq libnats fmt log4cpp)"
    read -r -a pkg_libs <<< "$(pkg-config --libs libpq libnats fmt log4cpp)"
    if ! g++ -std=c++23 -O0 -g -Wall -Wextra -Wpedantic \
        "$COMPARE_SRC" "$ROOT/research/src/realtest.cpp" \
        -I"$ROOT/research/src" "${includes[@]}" "${pkg_cflags[@]}" \
        "$BUILD/lib/src/libalgolib.so" "${pkg_libs[@]}" \
        -Wl,-rpath,"$BUILD/lib/src" -pthread \
        -o "$WORK_DIR/distributed_fast_compare" >"$LOG_DIR/03_comparator_compile.log" 2>&1; then
        echo "[FAIL] distributed-vs-fast comparator compile failed" >&2
        cat "$LOG_DIR/03_comparator_compile.log" >&2 || true
        return 1
    fi
}

printf '%s\n' "$window_output" >"$LOG_DIR/00_window.env"

# Export Compose interpolation variables once for the lifetime of this harness.
# This is deliberately done before any `docker compose` command so config, down,
# up, logs and cleanup all resolve the exact same host ports.
export HISTORICAL_DATA_PATH="$HISTORICAL_DATA"
export REPLAY_START_DATE="$REPLAY_START_DATE"
export REPLAY_END_DATE="$REPLAY_END_DATE"
export REPLAY_BARRIER_TIMEOUT_MS="$barrier_timeout_ms"
export REPLAY_NATS_PORT="$nats_port"
export REPLAY_NATS_MONITOR_PORT="$nats_monitor_port"
export REPLAY_POSTGRES_PORT="$postgres_port"
export REPLAY_PORTFOLIO_CONFIG="$portfolio_config"

echo "============================================================"
echo "HISTORICAL DISTRIBUTED VS FAST REPLAY"
echo "SOURCE       : $HISTORICAL_DATA"
echo "SOURCE RANGE : $SOURCE_FIRST_DATE .. $SOURCE_LAST_DATE"
echo "REPLAY       : $REPLAY_START_DATE .. $REPLAY_END_DATE ($EXPECTED_CYCLES cycles)"
echo "MEASURE      : $MEASURE_START_DATE .. $MEASURE_END_DATE ($MEASURE_DAYS days)"
echo "FINAL OPEN   : $FINAL_OPEN_DATE"
echo "PORTFOLIO    : $portfolio_mode"
echo "LOGS         : $LOG_DIR"
echo "============================================================"

ninja -C "$BUILD" -j8 >"$LOG_DIR/01_build.log" 2>&1
echo "[PASS] full ninja build"

bash "$DEPLOY/build_runtime_bundle.sh" >"$LOG_DIR/02_runtime_bundle.log" 2>&1
grep -Fq "Runtime bundle ready" "$LOG_DIR/02_runtime_bundle.log"
echo "[PASS] distributed runtime bundle assembled"

compile_comparator
echo "[PASS] distributed-vs-fast comparator compiled"

"${COMPOSE[@]}" down -v --remove-orphans >/dev/null 2>&1 || true

compose_up_mode=(--build)
if [[ "$force_runtime_build" != "1" ]] && docker image inspect "$runtime_image" >/dev/null 2>&1; then
    compose_up_mode=(--no-build)
    echo "[PASS] local runtime image reused: $runtime_image"
else
    echo "[INFO] local runtime image build required: $runtime_image"
fi

# Fail fast if Compose did not interpolate the requested host ports. This catches
# stale/unpatched compose files before Docker creates any container.
compose_config="$LOG_DIR/03b_compose_config.yml"
"${COMPOSE[@]}" config >"$compose_config"
if ! grep -Eq "published: ['\"]?$nats_port['\"]?$" "$compose_config"; then
    echo "[FAIL] Compose did not render requested NATS client port $nats_port"
    grep -n -A8 -B2 "target: 4222" "$compose_config" || true
    exit 1
fi
if ! grep -Eq "published: ['\"]?$nats_monitor_port['\"]?$" "$compose_config"; then
    echo "[FAIL] Compose did not render requested NATS monitor port $nats_monitor_port"
    grep -n -A8 -B2 "target: 8222" "$compose_config" || true
    exit 1
fi
if ! grep -Eq "published: ['\"]?$postgres_port['\"]?$" "$compose_config"; then
    echo "[FAIL] Compose did not render requested PostgreSQL port $postgres_port"
    grep -n -A8 -B2 "target: 5432" "$compose_config" || true
    exit 1
fi
echo "[PASS] Compose ports rendered: nats=$nats_port monitor=$nats_monitor_port postgres=$postgres_port"

start_ns="$(date +%s%N)"
if ! "${COMPOSE[@]}" up -d "${compose_up_mode[@]}" >"$LOG_DIR/04_compose_up.log" 2>&1; then
    echo "[FAIL] distributed compose startup failed"
    cat "$LOG_DIR/04_compose_up.log"
    dump_logs
    exit 1
fi
echo "[PASS] distributed historical replay topology started"

controller_id=""
for _ in $(seq 1 120); do
    controller_id="$("${COMPOSE[@]}" ps -q replay-controller 2>/dev/null || true)"
    [[ -n "$controller_id" ]] && break
    sleep 0.25
done
[[ -n "$controller_id" ]] || { echo "[FAIL] replay-controller container missing"; dump_logs; exit 1; }

# A long window can be deliberately slow with the correctness-first indicator path.
# Allow roughly barrier_timeout per cycle, capped by the loop cadence rather than a fixed 5-minute test limit.
deadline_epoch=$(( $(date +%s) + (EXPECTED_CYCLES * barrier_timeout_ms / 1000) + 120 ))
while true; do
    status="$(docker inspect -f '{{.State.Status}}' "$controller_id" 2>/dev/null || true)"
    if [[ "$status" == "exited" ]]; then
        exit_code="$(docker inspect -f '{{.State.ExitCode}}' "$controller_id")"
        [[ "$exit_code" == "0" ]] || { echo "[FAIL] replay-controller exited with code $exit_code"; dump_logs; exit 1; }
        break
    fi
    if (( $(date +%s) >= deadline_epoch )); then
        echo "[FAIL] replay-controller exceeded historical replay deadline"
        dump_logs
        exit 1
    fi
    sleep 0.5
done

end_ns="$(date +%s%N)"
if ! "${COMPOSE[@]}" logs replay-controller 2>&1 | grep -Fq "[REPLAY] finished cycles=$EXPECTED_CYCLES"; then
    echo "[FAIL] replay-controller did not report exactly $EXPECTED_CYCLES cycles"
    dump_logs
    exit 1
fi
elapsed_sec="$(awk -v a="$start_ns" -v b="$end_ns" 'BEGIN { printf "%.3f", (b-a)/1000000000.0 }')"
cycles_per_sec="$(awk -v n="$EXPECTED_CYCLES" -v s="$elapsed_sec" 'BEGIN { if (s>0) printf "%.3f", n/s; else print "0" }')"
echo "[PASS] historical replay completed: cycles=$EXPECTED_CYCLES elapsed=${elapsed_sec}s rate=${cycles_per_sec} cycles/s"

compare_args=(
    --nats-url "nats://127.0.0.1:$nats_port"
    --expected-cycles "$EXPECTED_CYCLES"
    --initial-cash "$initial_cash"
    --commission-rate "$commission_rate"
    --timeout-seconds 60
    --portfolio-mode "$portfolio_mode"
)
if [[ -n "$realtest_csv" ]]; then
    compare_args+=(--realtest-csv "$realtest_csv" --historical-data "$HISTORICAL_DATA")
    if [[ -n "$realtest_comparison_csv" ]]; then
        compare_args+=(--realtest-comparison-csv "$realtest_comparison_csv")
    fi
fi
[[ "$require_trading" == "1" ]] && compare_args+=(--require-trading)

compare_ok=0
if [[ -n "$realtest_csv" ]]; then
    # Keep stdout attached to the terminal because research's exact comparator is
    # intentionally interactive on mismatches. tee also preserves a validation log.
    set +e
    LD_LIBRARY_PATH="$BUILD/lib/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
        "$WORK_DIR/distributed_fast_compare" "${compare_args[@]}" 2>&1 \
        | tee "$LOG_DIR/05_distributed_fast_compare.log"
    compare_status=${PIPESTATUS[0]}
    set -e
    if [[ "$compare_status" == "0" ]]; then
        compare_ok=1
    fi
else
    for _ in $(seq 1 20); do
        if LD_LIBRARY_PATH="$BUILD/lib/src${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
            "$WORK_DIR/distributed_fast_compare" "${compare_args[@]}" \
            >"$LOG_DIR/05_distributed_fast_compare.log" 2>&1; then
            compare_ok=1
            break
        fi
        sleep 0.5
    done
fi
if [[ "$compare_ok" != "1" ]]; then
    echo "[FAIL] distributed-vs-fast historical comparator failed"
    cat "$LOG_DIR/05_distributed_fast_compare.log" 2>/dev/null || true
    dump_logs
    exit 1
fi
if [[ -z "$realtest_csv" ]]; then
    cat "$LOG_DIR/05_distributed_fast_compare.log"
fi
grep -Fq "DISTRIBUTED_FAST_COMPARE: PASS" "$LOG_DIR/05_distributed_fast_compare.log"
echo "[PASS] distributed and fast paths match across historical replay"
if [[ -n "$realtest_csv" ]]; then
    grep -Fq "DISTRIBUTED_REALTEST_RESEARCH_POLICY:" "$LOG_DIR/05_distributed_fast_compare.log"
    echo "[PASS] exact research RealTest comparison executed directly on distributed trades"
fi

if ! "${COMPOSE[@]}" exec -T postgres pg_isready -U algotrading -d algotrading >"$LOG_DIR/06_postgres_ready.log" 2>&1; then
    echo "[FAIL] PostgreSQL not healthy after historical replay"
    cat "$LOG_DIR/06_postgres_ready.log"
    exit 1
fi
echo "[PASS] PostgreSQL remains healthy"

for service in market-data strategy portfolio-risk order-planner execution-state exchange-gateway simulated-exchange; do
    cid="$("${COMPOSE[@]}" ps -q "$service")"
    status="$(docker inspect -f '{{.State.Status}}' "$cid")"
    [[ "$status" == "running" ]] || { echo "[FAIL] service not running: $service ($status)"; dump_logs; exit 1; }
done
echo "[PASS] all long-lived distributed services remain running"

echo
echo "============================================================"
echo "HISTORICAL REPLAY RESULT: PASS"
echo "Measured window : $MEASURE_START_DATE .. $MEASURE_END_DATE"
echo "Replay cycles   : $EXPECTED_CYCLES (warmup=$WARMUP_DAYS, measured=$MEASURE_DAYS)"
echo "Performance     : ${elapsed_sec}s / ${cycles_per_sec} cycles/s"
echo "Logs            : $LOG_DIR"
echo "============================================================"
