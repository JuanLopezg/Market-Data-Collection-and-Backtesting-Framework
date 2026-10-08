#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
LIVE_DIR="$ROOT/deploy/live"
COMPOSE="$LIVE_DIR/docker-compose.yml"
BUNDLE="$LIVE_DIR/build_runtime_bundle.sh"
ENV_EXAMPLE="$LIVE_DIR/.env.example"

fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

for f in "$COMPOSE" "$BUNDLE" "$ENV_EXAMPLE" "$LIVE_DIR/market_data_config.json"; do
    [[ -f "$f" ]] || bad "missing LIVE deploy file: $f"
done

if [[ ! -f "$COMPOSE" || ! -f "$BUNDLE" ]]; then
    exit 1
fi

# Production topology must not contain replay/fake-clock services/options.
# The shared bundler has an explicit --paper branch, targeting a different directory.
# Inspect default LIVE code after excluding only that guarded binary append block.
DEFAULT_BUNDLE="$(awk '/^if \[\[ "\$KIND" == PAPER \]\]; then$/ { paper=1; next } paper && /^fi$/ { paper=0; next } !paper { print }' "$BUNDLE")"
if grep -Eni 'replay-controller|algotrading_replay_controller|simulated-exchange|algotrading_simulated_exchange_service|--runtime-mode|--simulation-id|CLOCK_STATE|CLOCK_CONTROL|CLOCK_SYNC_REQUEST|REPLAY_' "$COMPOSE" "$ENV_EXAMPLE" ||
   grep -Eni 'replay-controller|algotrading_replay_controller|simulated-exchange|algotrading_simulated_exchange_service|--runtime-mode|--simulation-id|CLOCK_STATE|CLOCK_CONTROL|CLOCK_SYNC_REQUEST|REPLAY_' <<<"$DEFAULT_BUNDLE"; then
    bad "LIVE deploy contains replay/fake-clock dependency markers"
fi

# LIVE bundle must contain the six intended binaries and no replay/simulated venue binary.
for binary in \
    algotrading_market_data_service \
    algotrading_strategy_service \
    algotrading_portfolio_risk_service \
    algotrading_order_planner_service \
    algotrading_execution_state_service \
    algotrading_exchange_gateway; do
    grep -q "$binary" "$BUNDLE" || bad "LIVE bundle script does not package $binary"
done

# Only market-data is allowed to mount the canonical market directory RW.
python3 - "$COMPOSE" <<'PY'
import sys
from pathlib import Path
try:
    import yaml
except Exception as exc:
    print(f"FAIL: PyYAML is required for deploy audit: {exc}", file=sys.stderr)
    raise SystemExit(2)

path = Path(sys.argv[1])
data = yaml.safe_load(path.read_text())
services = data.get("services", {})
required = {"nats", "postgres", "market-data", "strategy", "portfolio-risk", "execution-state", "order-planner", "exchange-gateway"}
missing = sorted(required - set(services))
if missing:
    print("FAIL: missing LIVE compose services: " + ", ".join(missing), file=sys.stderr)
    raise SystemExit(3)

for forbidden in ("replay-controller", "simulated-exchange"):
    if forbidden in services:
        print(f"FAIL: forbidden default LIVE service exists: {forbidden}", file=sys.stderr)
        raise SystemExit(4)

rw = []
market_mount_services = []
for name, svc in services.items():
    for volume in svc.get("volumes", []) or []:
        text = volume if isinstance(volume, str) else str(volume)
        if "/data/market" not in text:
            continue
        market_mount_services.append(name)
        if text.endswith(":rw") or ":rw" in text:
            rw.append(name)

if rw != ["market-data"]:
    print(f"FAIL: canonical market directory RW owners must be exactly ['market-data']; got {rw}", file=sys.stderr)
    raise SystemExit(5)

for reader in ("strategy", "portfolio-risk", "execution-state"):
    if reader not in market_mount_services:
        print(f"FAIL: expected canonical market DB read mount missing from {reader}", file=sys.stderr)
        raise SystemExit(6)

if "order-planner" in market_mount_services:
    print("FAIL: order-planner must not mount canonical SQLite directly", file=sys.stderr)
    raise SystemExit(7)
if "exchange-gateway" in market_mount_services:
    print("FAIL: exchange-gateway must not mount canonical SQLite directly in STEP 6E", file=sys.stderr)
    raise SystemExit(8)

profiles = services["exchange-gateway"].get("profiles", []) or []
if "exchange-edge" not in profiles:
    print("FAIL: exchange-gateway must remain opt-in behind exchange-edge profile before STEP 7", file=sys.stderr)
    raise SystemExit(9)

print("PASS: LIVE deploy topology YAML ownership/profile checks passed.")
PY
rc=$?
[[ $rc -eq 0 ]] || fail "LIVE deploy YAML audit failed"

if [[ $fail -ne 0 ]]; then
    exit 1
fi

echo "PASS: LIVE build/deploy cleanup audit passed."
