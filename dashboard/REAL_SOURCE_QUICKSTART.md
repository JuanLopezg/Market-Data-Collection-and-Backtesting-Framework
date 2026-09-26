# Real-source quickstart (Step 18)

Keep the normal mock run available with:

```bash
docker compose up -d --build
```

To run the real Infrastructure + Positions pages:

```bash
# From the project root
cd deploy/live
docker compose up -d postgres nats

# Then from dashboard/
cd ../../dashboard
docker compose \
  --env-file ../deploy/live/.env \
  -f docker-compose.yml \
  -f docker-compose.real.yml \
  up -d --build --force-recreate
```

Login at:

```text
http://localhost:8080
```

Then open **Infrastructure** and **Positions**. Infrastructure reads real source diagnostics; Positions reads the durable ExecutionState snapshot. If the runtime snapshot currently has zero physical positions, the Positions table will correctly be empty.

Useful authenticated diagnostics in the same browser session:

```text
http://localhost:8080/api/source-status
http://localhost:8080/api/runtime-state-summary
```

If only PostgreSQL and NATS are running, `trading_runtime_state` may legitimately be absent. Infrastructure will still load and will show the runtime state as unknown/not populated instead of failing.

To return to mocks:

```bash
docker compose -f docker-compose.yml down --remove-orphans
docker compose -f docker-compose.yml up -d --build --force-recreate
```


## Step 18 update
`Execution` now has a bounded real read model from `trading_runtime_state.snapshot.orders` + `trading_fills`. Latency/slippage/replacement lineage remain unavailable unless a canonical durable source is added.


## Step 19 check

After logging in, open `/reconciliation`. If an exchange snapshot has not yet been retained, `PENDING` is expected. To produce fresh canonical exchange evidence, run the normal trading topology that owns reconciliation (ExecutionState plus the configured exchange path); do not use the dashboard to synthesize or publish a snapshot request.


## Step 20 — Pipeline / Why

Real Pipeline lineage is now read-only and cycle-aligned across the durable Strategy, PortfolioRisk and OrderPlanner checkpoints, with ExecutionState/reconciliation enrichment. The risk checkpoint is the anchor because it persists the exact StrategyIntentBatch and AccountSnapshot used to produce the DecisionBatch. Missing planner data is `PENDING`; no cross-cycle mixing is allowed. RSI/rank and intermediate risk transforms remain unavailable unless a canonical persisted source is added.
