# LIVE pre-exchange topology (STEP 6E)

This directory is the production-path deployment scaffold after the fake/replay clock cleanup.
It is intentionally separate from `deploy/distributed_replay/`.

The default topology starts only the exchange-agnostic LIVE path:

`MarketData -> Strategy -> PortfolioRisk -> ExecutionState -> OrderPlanner`

It does **not** start replay-controller or simulated-exchange, and it does **not** submit real exchange orders.
`exchange-gateway` is packaged but is behind the optional Compose profile `exchange-edge` until STEP 7/8.

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
ninja -C build -j8
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

STEP 7 will define the dry-run exchange preparation boundary. STEP 8 will be the first stage allowed to add authenticated Hyperliquid submission.

## STEP 6F — final pre-exchange acceptance

Before attaching STEP 7, run the structural gate:

```bash
bash validation/live_pre_exchange_e2e_audit.sh .
```

After a real daily cycle `T` has propagated through the running Compose topology, run:

```bash
bash validation/live_pre_exchange_runtime_acceptance.sh . YYYYMMDD
```

The acceptance boundary is still `NotionalOrderPlan`; no real exchange order should be emitted. See `STEP6F_APPLY.md` for the required same-date event chain and restart/duplicate gate.
