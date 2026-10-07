# LIVE pre-exchange deployment

This directory is the production-path deployment scaffold after the fake/replay clock cleanup.
The former shared-clock `deploy/distributed_replay/` topology has been removed.
Run commands below from the repository root under WSL.

The default topology starts only the exchange-agnostic LIVE path:

`MarketData -> Strategy -> PortfolioRisk -> ExecutionState -> OrderPlanner`

It does **not** start replay-controller or simulated-exchange, and it does **not** submit real exchange orders.
`exchange-gateway` is packaged but is behind the optional Compose profile `exchange-edge` while private exchange integration remains unfinished.

## One-time preparation

Copy the environment template and set a real PostgreSQL password:

```bash
cp deploy/live/.env.example deploy/live/.env
# edit deploy/live/.env
```

Make sure the host market-data directory exists:

```bash
mkdir -p storage/databases
```

For a brand-new database, initialize the canonical market DB before starting all read-only consumers:

```bash
# Build first (see below), then bring up infrastructure only.
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml up -d nats postgres

# Run exactly one market-data update. This creates/updates the SQLite schema and
# commits the previous completed UTC day before publishing market.data.updated.v1.
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml run --rm \
  market-data algotrading_market_data_service \
  --config /opt/algotrading/live/market_data_config.json --run-once
```

If `storage/databases/database.db` already contains the canonical schema, the bootstrap command is optional.

## Build the LIVE runtime image inputs

From the repository root:

```bash
meson compile -C build
bash deploy/live/build_runtime_bundle.sh
```

The generated `.runtime_bundle/` contains only the six LIVE binaries:

- market-data
- strategy
- portfolio-risk
- order-planner
- execution-state
- exchange-gateway

It deliberately excludes replay-controller and simulated-exchange.

## Run the pre-exchange LIVE pipeline

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml up --build -d
```

At this stage the acceptance target ends at `NotionalOrderPlan`. No real venue order is sent.

To inspect logs:

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml logs -f \
  market-data strategy portfolio-risk execution-state order-planner
```

Stop without deleting durable PostgreSQL/JetStream state:

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml down
```

Do not use `down -v` unless you intentionally want to delete the LIVE Docker volumes.

## Exchange gateway profile

The gateway is not part of the default pre-exchange run. It can be started explicitly for transport-only testing:

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml \
  --profile exchange-edge up -d exchange-gateway
```

This profile starts the gateway transport service only; it does not add a private
venue implementation or enable real submit/cancel/fill lifecycle. Dashboard public
TESTNET metadata/dry-run configuration is separate evidence, not a connected backend.

## Validate the pre-exchange boundary

Run the current structural gate:

```bash
bash validation/live_pre_exchange_e2e_audit.sh .
```

After a real daily cycle `T` has propagated through the running Compose topology, run:

```bash
bash validation/live_pre_exchange_runtime_acceptance.sh . YYYYMMDD
```

The verifier checks the same `YYYYMMDD` in durable MarketData/Strategy,
AccountSnapshot, Risk decision and Planner request/plan evidence, exactly one checkpoint
per stage, and unchanged reference-close maps. Its source is
`validation/live_pre_exchange_runtime_acceptance.sh`.

The acceptance boundary remains `NotionalOrderPlan`. This verifier does not
perform a complete restart/duplicate/broker-failure campaign. Canonical replay restart
is tested separately by Step59; deployed service recovery remains its own acceptance
work. See [CURRENT_STATE.md](../../CURRENT_STATE.md) and
[validation/README.md](../../validation/README.md).

## VPS log retention (future deployment task)

Before final VPS deployment, implement and verify:

- [ ] Persist diagnostic logs in a host directory that survives container restart/recreation.
- [ ] Rotate daily into dated files per service, retaining the latest five days.
- [ ] Automatically delete older diagnostic log files; keep disk usage bounded.
- [ ] Preserve timestamps, service names and error context for troubleshooting; never log secrets.
- [ ] Test day-boundary rotation, five-day cleanup and restart behavior on the VPS.

Current Docker/application rotation is size-based; five retained files do not mean
five retained days. This daily retention policy is planned, not implemented yet.
Apply housekeeping to diagnostic logs, not trading journals, fills, checkpoints or
other durable audit/accounting evidence.
