#!/usr/bin/env bash
set -euo pipefail

ROOT="${1:-.}"
COMPOSE="$ROOT/deploy/historical_replay/docker-compose.yml"
ENV_EXAMPLE="$ROOT/deploy/historical_replay/.env.example"
README="$ROOT/deploy/historical_replay/README.md"

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "PASS: $*"; }

[[ -f "$COMPOSE" ]] || fail "missing $COMPOSE"
[[ -f "$ENV_EXAMPLE" ]] || fail "missing $ENV_EXAMPLE"
[[ -f "$README" ]] || fail "missing $README"

echo "============================================================"
echo "T14 — HISTORICAL REPLAY ISOLATED TOPOLOGY AUDIT"
echo "============================================================"

python3 - "$COMPOSE" "$ROOT" <<'PY'
import sys
from pathlib import Path
import yaml

path = Path(sys.argv[1])
with path.open("r", encoding="utf-8") as fh:
    doc = yaml.safe_load(fh)

if doc.get("name") != "algotrading-historical-replay":
    raise SystemExit("compose project name is not isolated")

services = doc.get("services", {})
required = {
    "nats", "postgres", "historical-market-data", "strategy", "portfolio-risk",
    "execution-state", "order-planner", "exchange-gateway", "simulated-exchange",
}
missing = sorted(required - services.keys())
if missing:
    raise SystemExit(f"missing services: {missing}")
if "replay-controller" in services:
    raise SystemExit("legacy replay-controller must not exist in new topology")

runtime = doc.get("x-runtime", {})
if runtime.get("env_file") != ["./run/time.env"]:
    raise SystemExit("x-runtime must load exactly ./run/time.env")

network_names = {v.get("name") for v in doc.get("networks", {}).values() if isinstance(v, dict)}
if "${HISTORICAL_REPLAY_NETWORK:-algotrading-historical-replay-net}" not in network_names:
    raise SystemExit("historical replay network is not explicitly isolated")

volumes = doc.get("volumes", {})
for name in ("nats-data", "postgres-data", "market-data-db"):
    if name not in volumes:
        raise SystemExit(f"missing isolated volume {name}")

feeder_volumes = services["historical-market-data"].get("volumes", [])
if not any(str(v).endswith(":/data/historical/1d_cmc.csv:ro") for v in feeder_volumes):
    raise SystemExit("historical dataset must be mounted read-only")
if "market-data-db:/data/market:rw" not in feeder_volumes:
    raise SystemExit("historical feeder must be the market SQLite writer")

# Current consumers need writable WAL sidecar/lock files in the volume.
# Their canonical SQLite connection itself must remain read-only/query-only.
root = Path(sys.argv[2])
canonical_reader = (root / "lib/src/market/canonical_market_data_reader.cpp").read_text(encoding="utf-8")
for marker in ("SQLITE_OPEN_READONLY", "PRAGMA query_only=ON"):
    if marker not in canonical_reader:
        raise SystemExit(f"canonical SQLite reader lost its read-only guard: {marker}")

reader_sources = {
    "strategy": "strategy_service",
    "portfolio-risk": "portfolio_risk_service",
    "execution-state": "execution_state_service",
}
for reader, section in reader_sources.items():
    mounts = services[reader].get("volumes", [])
    if not any(v in mounts for v in ("market-data-db:/data/market:ro", "market-data-db:/data/market:rw")):
        raise SystemExit(f"{reader} must use the isolated canonical market SQLite volume")
    source = (root / "live_trading" / section / "src" / f"{section}_main.cpp").read_text(encoding="utf-8")
    if "CanonicalMarketDataReader" not in source:
        raise SystemExit(f"{reader} must use the shared read-only canonical reader")

for svc in ("strategy", "portfolio-risk", "execution-state", "order-planner", "exchange-gateway", "simulated-exchange"):
    if services[svc].get("profiles") != ["runtime"]:
        raise SystemExit(f"{svc} must inherit the T14 runtime profile")

if services["historical-market-data"].get("profiles") != ["runtime"]:
    raise SystemExit("historical-market-data must join the runtime profile after T15")

print("yaml-structure-ok")
PY
pass "compose YAML structure and isolation invariants"

FORBIDDEN='ServiceClockContext|SimulatedClock|ClockState|ClockControl|ClockSyncRequest|simulation\.clock\.|--runtime-mode|--simulation-id|replay-controller'
if grep -En "$FORBIDDEN" "$COMPOSE"; then
    fail "legacy shared-clock residue found in new compose"
fi
pass "no legacy shared-clock control plane in new compose"

for stream in ALGOTRADING_HISTORICAL_RUNTIME ALGOTRADING_HISTORICAL_EXCHANGE_CONTROL ALGOTRADING_HISTORICAL_EXCHANGE_BACKEND; do
    grep -q "$stream" "$COMPOSE" || fail "missing isolated stream name $stream"
done
pass "historical replay uses isolated stream names"

grep -q 'HISTORICAL_REPLAY_NATS_PORT=64228' "$ENV_EXAMPLE" || fail "isolated NATS port missing"
grep -q 'HISTORICAL_REPLAY_NATS_MONITOR_PORT=65228' "$ENV_EXAMPLE" || fail "isolated NATS monitor port missing"
grep -q 'HISTORICAL_REPLAY_POSTGRES_PORT=65438' "$ENV_EXAMPLE" || fail "isolated PostgreSQL port missing"

python3 - "$ENV_EXAMPLE" <<'PYPORTS'
import sys
from pathlib import Path

keys = {
    "HISTORICAL_REPLAY_NATS_PORT",
    "HISTORICAL_REPLAY_NATS_MONITOR_PORT",
    "HISTORICAL_REPLAY_POSTGRES_PORT",
}
values = {}
for raw in Path(sys.argv[1]).read_text(encoding="utf-8").splitlines():
    line = raw.strip()
    if not line or line.startswith("#") or "=" not in line:
        continue
    key, value = line.split("=", 1)
    if key in keys:
        try:
            port = int(value)
        except ValueError:
            raise SystemExit(f"{key} is not an integer: {value}")
        if not 1 <= port <= 65535:
            raise SystemExit(f"{key} is outside valid TCP port range: {port}")
        values[key] = port

missing = keys - values.keys()
if missing:
    raise SystemExit(f"missing host ports: {sorted(missing)}")
if len(set(values.values())) != len(values):
    raise SystemExit(f"historical replay host ports must be unique: {values}")
print("host-ports-ok")
PYPORTS
pass "host inspection ports are isolated, unique, and within 1..65535"

echo "PASS: T14 historical replay isolated topology audit"
