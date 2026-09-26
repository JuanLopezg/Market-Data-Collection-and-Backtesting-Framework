# Historical replay topology — T15

This deployment is isolated from LIVE and from the legacy `deploy/distributed_replay` topology.
It has its own Docker network, NATS JetStream volume, PostgreSQL volume, canonical market-data
SQLite volume, stream names, and host inspection ports.

There is **no replay-controller and no shared logical clock**. Every business service inherits the
same `./run/time.env` file generated once by T13.

## Historical feeder semantics

`algotrading_historical_market_data_service` reads the reference CSV sequentially. It does not load
the full 2020-2025 dataset into the canonical runtime database. For each source day it first checks
that the day is fully visible according to `TimeHandler`, then it parses and commits exactly that
visible day to the canonical replay SQLite, and only after COMMIT publishes `market.data.updated.v1`.

The source identity defaults to `historical-cmc`. Strategy and ExecutionState receive that same
identity explicitly through `--market-data-source`; LIVE keeps `binance` as their default.

The historical topology uses a deliberately high `HISTORICAL_MARKET_TOP_N` so all symbols present
for the day enter the canonical ranking candidate set. The strategy's own configured liquidity
universe (for PureRSI, top-N by its indicator logic) therefore remains responsible for selecting
the actual entry universe instead of an arbitrary feeder-side truncation.

## Prepare one run

```bash
cp deploy/historical_replay/.env.example deploy/historical_replay/.env
# edit POSTGRES_PASSWORD and HISTORICAL_DATA_PATH if needed

mkdir -p deploy/historical_replay/run
python3 tools/historical_replay/create_time_env.py \
  --speed 100 \
  --simulated-reference 2020-01-01T00:00:00Z \
  --output deploy/historical_replay/run/time.env
```

The generated time file must be created **once per run**, not once per container.

## Validate T15

```bash
./validation/historical_replay_topology_audit.sh .
./validation/historical_market_data_source_unit_test.sh .
./validation/historical_market_data_feeder_audit.sh .

docker compose \
  --env-file deploy/historical_replay/.env \
  -f deploy/historical_replay/docker-compose.yml \
  config >/tmp/algotrading-historical-replay.compose.yml
```

Then run the normal project build. Once the new feeder binary exists, package the runtime:

```bash
./deploy/historical_replay/build_runtime_bundle.sh

docker compose \
  --env-file deploy/historical_replay/.env \
  -f deploy/historical_replay/docker-compose.yml \
  --profile runtime build
```

T15 does not yet declare the whole distributed replay economically accepted. T16 adds explicit
anti-lookahead acceptance checks before T17 runs a reference comparison.

## Isolation invariants

- No `replay-controller`.
- No `ClockState`, `ClockControl`, `ClockSyncRequest`, `ServiceClockContext` or clock subjects.
- No `--runtime-mode` / `--simulation-id`.
- NATS/PostgreSQL/market SQLite state is not shared with LIVE.
- Historical source is mounted read-only.
- Only the historical feeder writes canonical replay market SQLite.
- Strategy, PortfolioRisk and ExecutionState mount canonical SQLite read-only.
- Retry/poll sleeps remain technical real time; candle visibility is business time.
