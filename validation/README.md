# Unified validation

Run the complete current economic regression suite from project root:

```bash
bash validation/run_validation.sh
```

The single script performs, in order:

1. PureRSI + EqualWeight full-history distributed replay.
2. Exact distributed-vs-fast comparison.
3. Exact `research/src/realtest.cpp` comparison against `pureRSI.csv`.
4. Locked known RealTest baseline check (only the confirmed BNB/FET/ZEC exceptions).
5. PureRSI + VolTarget full-history distributed replay.
6. Exact distributed-vs-fast comparison.
7. Exact research RealTest campaign comparison.
8. Locked known VolTarget RealTest baseline check.
9. PostgreSQL and long-lived service health gates for both runs.

The historical source defaults to `storage/databases/1d_cmc.csv` and the external RealTest source defaults to `storage/backtests/final_tests/pureRSI.csv`.

Environment overrides remain available:

- `HISTORICAL_DATA_PATH=/path/to/1d_cmc.csv`
- `REALTEST_CSV_PATH=/path/to/pureRSI.csv`
- `HISTORICAL_WARMUP_DAYS=30`
- `HISTORICAL_KEEP_ON_FAILURE=0|1` (defaults to `1` in this master validation)

The master validation sets `REALTEST_NONINTERACTIVE=1`, which only skips the manual ENTER prompts for mismatches. The matching and validation policy remains the exact research implementation. Normal research runs stay interactive.

Logs are retained under:

- `validation/logs/equal_weight/<timestamp>/`
- `validation/logs/vol_target/<timestamp>/`

## STEP 33A — Strategy restartability

Short production-like restart test:

```bash
bash validation/restart_strategy.sh
```

It runs a 90-cycle EqualWeight replay, waits until at least 45 execution barriers,
then hard-kills and deletes the Strategy container and recreates it. Strategy must
rebuild its rolling market state and restore its exact last signal snapshot from
PostgreSQL-checkpointed slice+intent rows,
resume the same durable JetStream consumer, and still finish with
`DISTRIBUTED_FAST_COMPARE: PASS`.

The test intentionally recreates the entire container, so passing cannot depend on
files left in the old container writable layer.

## STEP 33B — ReplayController restartability

Mid-phase hard restart test:

```bash
bash validation/restart_replay_controller.sh
```

It runs a 90-cycle EqualWeight replay, pauses `order-planner` after at least 45
completed execution barriers so the next cycle is held in `WAIT_EXECUTION`, then
hard-kills and deletes `replay-controller` and recreates it.

ReplayController checkpoints observed DecisionBatch and ExecutionCycleComplete
barriers in PostgreSQL **before ACKing them**. On recreation it must recover the
completed prefix plus the in-flight decision, skip already completed cycles,
resume the pending execution phase, finish exactly 90 cycles, and still produce
`DISTRIBUTED_FAST_COMPARE: PASS`.

The historical harness option `--allow-controller-recreate` exists only so the
validation harness can follow the replacement container instead of treating the
intentional hard restart as a test-harness failure.


## STEP 33C — SimulatedExchange restartability

Independent exchange-truth hard restart test:

```bash
bash validation/restart_simulated_exchange.sh
```

It runs a 90-cycle EqualWeight replay and waits until the simulated exchange has
already persisted real trading state (execution prices, known orders and at least
one generated FillID). It then hard-kills and deletes the entire
`simulated-exchange` container and recreates it.

The recreated exchange must recover **its own** PostgreSQL checkpoint:
- exchange cash and positions,
- exact next FillID,
- known orders,
- active orders,
- already released execution prices,
- durable backend-event outbox.

Recovery does not copy state from ExecutionState. The replay must resume, the
durable prefix must not regress, no duplicate outbox message identity may appear,
and the final result must still be `DISTRIBUTED_FAST_COMPARE: PASS`.

## STEP 33D — Remaining-service restart audit

Run one audited service at a time:

```bash
bash validation/restart_remaining_services.sh portfolio-risk
bash validation/restart_remaining_services.sh order-planner
bash validation/restart_remaining_services.sh execution-state
bash validation/restart_remaining_services.sh exchange-gateway
bash validation/restart_remaining_services.sh market-data
```

Or run all five sequentially:

```bash
bash validation/restart_remaining_services.sh
```

Audit result encoded by this step:

- `portfolio-risk`: **stateful**. Its already-ACKed market slices, account snapshots and
  last produced decision timestamp are now checkpointed in PostgreSQL. Recovery rebuilds
  RollingMarketState/account barriers and restores the engine timestamp before consuming
  new StrategyIntentBatch messages.
- `order-planner`: **stateless by design**. Every planning request contains the complete
  immutable planning snapshot and state revision required to reproduce the output.
- `execution-state`: **stateful but already durable**. The restart test requires the
  PostgreSQL state restore path followed by a clean exchange reconciliation.
- `exchange-gateway`: **stateless protocol boundary in distributed replay**. Durable
  command ownership remains in JetStream and the simulated exchange backend now owns
  independent idempotent exchange truth/outbox state.
- `market-data`: **reconstructible**. Historical source data is immutable/read-only and
  release commands are durable; the service owns no executed trading state.

The `portfolio-risk` case deliberately runs **VolTarget**, making loss of rolling market/covariance history observable in the exact comparator. The other service cases use EqualWeight.

Each hard-restart case deletes the old container, creates a new one, requires replay
progress after recreation, rejects any `reconciliation_blocked` result, and finishes only
when `DISTRIBUTED_FAST_COMPARE: PASS` remains exact.

## STEP 33E — Consolidated restart regression suite

Run the complete restart regression suite:

```bash
bash validation/restart_suite.sh
```

Default `all` scope executes, sequentially:

1. STEP 33A Strategy hard restart/recreate.
2. STEP 33B ReplayController mid-phase hard restart/recreate.
3. STEP 33C SimulatedExchange independent-truth hard restart/recreate.
4. STEP 33D hard restart audit for PortfolioRisk, OrderPlanner, ExecutionState,
   ExchangeGateway and MarketData.

The first executed restart case forces the shared runtime image rebuild. Remaining
cases reuse that exact image while still using independent Compose projects and
fresh PostgreSQL/NATS volumes. This avoids rebuilding the same image for every
case without weakening restart isolation.

For focused execution:

```bash
bash validation/restart_suite.sh core
bash validation/restart_suite.sh remaining
```

- `core`: only already-established 33A/33B/33C restart paths.
- `remaining`: all 33D service cases through the 33E orchestrator. This is useful
  immediately after applying PATCH 33D because it validates STEP 33D and the new
  suite wiring together without rerunning already-closed 33A/33B/33C.

Each child test preserves its existing strong oracle: hard container deletion and
recreation, replay progress, service-specific recovery checks, no reconciliation
block where applicable, and final `DISTRIBUTED_FAST_COMPARE: PASS`.

Suite-level logs are written under:

```text
validation/logs/restart_suite/<timestamp>/
```

Standalone restart scripts still force a runtime build by default. The environment
variable `RESTART_SKIP_RUNTIME_BUILD=1` is an internal suite optimization and should
only be used when the current runtime image has already been rebuilt from the same
source tree.


## STEP 34 — Failure / chaos suite

Run the complete failure suite:

```bash
bash validation/chaos_suite.sh all
```

Focused scopes are also available:

```bash
bash validation/chaos_suite.sh infra
bash validation/chaos_suite.sh delivery
bash validation/chaos_suite.sh execution
bash validation/chaos_suite.sh safety
```

The suite uses isolated 90-cycle historical replay projects and deliberately injects:

- hard NATS outage/restart while JetStream state remains on its volume;
- hard PostgreSQL outage/restart followed by clean process reconnect/recovery of
  PostgreSQL-owning services;
- Fill redelivery after ExecutionState has already persisted the fill but before ACK;
- a second transport message carrying an already-processed business `FillID`;
- one deliberately stale `OrderPlanBatch.state_revision`;
- SimulatedExchange process crash after durable state/outbox COMMIT but before publish;
- a one-order split/partial-fill lifecycle;
- a one-order exchange rejection;
- a deliberate local/exchange reconciliation mismatch.

For scenarios that should be economically invisible (outages, redelivery, duplicate
FillID, stale plan and outbox crash), the normal exact oracle remains mandatory:

```text
DISTRIBUTED_FAST_COMPARE: PASS
```

Partial-fill and reject scenarios intentionally change the exchange event sequence, so
they use `--skip-fast-compare` and instead require their specific lifecycle/safety
invariants plus successful replay completion and no reconciliation block.

The reconciliation-mismatch scenario is intentionally a safe-failure test: it corrupts
only the SimulatedExchange independent durable truth, recreates both exchange and
ExecutionState, and requires:

```text
event=reconciliation_blocked
trading_state=paused
```

It is a PASS only when the system refuses to silently continue.

All chaos hooks are disabled by default and are exposed only through
`ALGOTRADING_CHAOS_*` environment variables used by the validation harness. Normal
backtests and live-like replay remain unchanged when those variables are absent/zero.

### Final gate before dashboard

To rerun the entire restart suite and then the entire chaos suite:

```bash
bash validation/pre_dashboard_suite.sh
```

The final expected marker is:

```text
PRE-DASHBOARD INFRA VALIDATION: PASS
```

Only after this gate passes should the project move to STEP 35 / Trading Control
Dashboard.
