# Historical replay — current canonical entry point

For current research acceptance, use the single public replay wrapper rather than invoking the older distributed T15 topology directly:

```bash
python3 research/replay.py {fast|system|dashboard}
```

Current canonical source dataset: `deploy/historical_replay/run/1d_cmc_by_date.csv`, derived from the historical source and covering the accepted full-history window `2020-01-01..2025-10-13`.

The `realtest-parity` profile preserves original OHLCV and reproduces the old Backtester next-open execution economics with zero parity fees/slippage and no execution volume-capacity limit. `system` and `dashboard` have been validated to the same full-history fingerprint:

```text
1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0
```

`dashboard` is the same economic replay plus state publication. Restart/resume is supported for both `system` and `dashboard` through `--checkpoint-every`, `--stop-after-days`, and `--resume`.

Important: `--start` starts the simulation cold at that date. To keep prior warm-up/history and slow only a later interval, use `--pace-start` / `--pace-end` instead.

See root `CURRENT_STATE.md` and `research/REPLAY.md` for the active contract.

---

# Legacy distributed historical replay topology — T15

The section below documents the older isolated distributed-replay topology. It is retained as historical architecture/validation material and is not the current RealTest acceptance entry point.

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
