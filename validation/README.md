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

### Final infrastructure gate before the clock phase

To rerun the entire restart suite and then the entire chaos suite:

```bash
bash validation/pre_dashboard_suite.sh
```

The final expected marker remains:

```text
PRE-DASHBOARD INFRA VALIDATION: PASS
```

The marker name is retained for compatibility with the existing validation scripts and
artifacts. Passing it closes the restartability/chaos infrastructure gate; it does **not**
mean the dashboard should be implemented immediately.

After the stable economic baseline has been frozen, the next phase is **STEP 35 — Clock
Audit / Shared Logical Clock**:

1. Audit every current time source and classify it as business/event time or technical
   monotonic/process time.
2. Verify/reuse the common `Clock` abstraction and keep LIVE/TESTNET on a simple
   `SystemClock` with no fake-clock infrastructure dependency.
3. Add the REPLAY-only shared logical/simulated clock where required, preserving the
   existing causal barriers.
4. Validate clock monotonicity, restart recovery, pause/resume and accelerated/MAX replay
   with the same economic result and exact `distributed == fast` where applicable.

Only after the clock validation passes should the project move to the Trading Control
Dashboard implementation.

## STEP 35A — Clock audit

Before adding any shared/fake time infrastructure, run the static clock audit:

```bash
bash validation/clock_audit.sh
```

Expected result on the frozen pre-clock baseline:

```text
STEP 35A CLOCK AUDIT RESULT: PASS
Audit only. This PASS does not by itself close the shared-clock implementation/validation phase.
```

The command also writes `validation/CLOCK_AUDIT.md` with the classified findings.
This audit deliberately distinguishes:

- business/event time, which must not be sourced directly from host wall-clock inside
  the distributed trading services or runtime business core;
- technical/process time such as replay barrier deadlines, network backoff, validation
  harness sleeps, file mtimes and persistence `updated_at` metadata;
- the explicit `SystemClock` boundary used by LIVE/TESTNET.

`PASS` here closes only the **audit** sub-step. It does not claim that `SimulatedClock`,
`ClockState`, shared logical time, speed control or restart synchronization exist yet.
Those are the next implementation step.

## STEP 35B — Shared logical clock foundation / replay authority

After `STEP 35A CLOCK AUDIT RESULT: PASS`, apply the logical-clock foundation and run:

```bash
bash validation/shared_clock_foundation_check.sh
```

This sub-step adds the stable `ClockState` wire contract, the mutable monotonic
`SimulatedClock`, the `simulation.clock.state.v1` JetStream subject, and a PostgreSQL-backed
clock authority inside `replay-controller`. The authority persists each new clock revision
before publishing it and advances logical time immediately before the existing CLOSE(T) and
OPEN(T+1) market releases. Existing `steady_clock` barrier deadlines remain technical time.

`SystemClock` is deliberately unchanged as the simple LIVE/TESTNET boundary. The new NATS
clock subject is REPLAY infrastructure; this sub-step does not add any LIVE dependency on it.

Expected marker:

```text
STEP 35B FOUNDATION CHECK: PASS
```

Important: this closes only the **foundation/authority** slice. It does not yet mean all
runtime Dockers consume the same logical time. The next sub-step is **STEP 35C**: wire
REPLAY-only clock followers into the distributed services, enforce causal clock-before-event
processing, and validate restart resynchronization. Speed/pause controls and x1/MAX economic
equivalence remain later clock-validation work.

## STEP 35C — REPLAY clock followers + causal clock-before-event synchronization

After `STEP 35B FOUNDATION CHECK: PASS`, wire every distributed REPLAY service to the
shared authority and run:

```bash
bash validation/shared_clock_followers_suite.sh
```

This sub-step introduces a bootstrap-only `RuntimeMode` and `ServiceClockContext`:

- LIVE/TESTNET default to `SystemClock` and create no fake-clock consumer/request;
- REPLAY creates a `SimulatedClock` follower for each service;
- every follower consumes `simulation.clock.state.v1` through its own durable consumer;
- business deliveries are held with `Retry` until the local logical clock authorizes the
  contract `metadata.produced_at` timestamp;
- the private simulated-exchange -> exchange-gateway event path uses the same gate;
- a restarted follower publishes `simulation.clock.sync.request.v1`, allowing the replay
  authority to re-publish its latest durable `ClockState` even when that follower had
  already ACKed all earlier clock messages;
- `replay-controller` also installs every durable `ClockState` into its local
  `SimulatedClock`, so the authority is part of the same common clock abstraction.

The runtime stream bootstrap is now mode-sensitive. REPLAY includes clock subjects;
LIVE/TESTNET uses only normal trading subjects so fake-clock NATS subjects are not a
production clock dependency.

The suite performs three gates:

1. static architectural/source checks;
2. a real incremental `ninja` build plus the original clock audit, which must report
   `runtime Clock consumers : 8/8` and zero blocking business findings;
3. a short historical distributed replay that must remain exactly `distributed == fast`
   and prove `event=clock_synchronized` in all seven follower service logs.

Expected final marker:

```text
STEP 35C CLOCK FOLLOWERS RESULT: PASS
```

This still does **not** close the complete shared-clock phase. The next sub-step is
**STEP 35D**: inject hard follower restarts while replay is already in flight and prove the
sync-request/re-publish path restores the exact active `simulation_id`, `logical_time` and
`revision` without allowing a future event to execute against a stale clock. Accelerated
x1/multiplier/MAX and pause/resume equivalence remain subsequent validation work.

## STEP 35D — Hard-restart follower re-sync + clock causality recovery proof

After `STEP 35C CLOCK FOLLOWERS RESULT: PASS`, this sub-step hardens and proves the existing
re-sync path under real container destruction/recreation while a replay is in flight. A REPLAY
follower now retries the sync request once per second of technical monotonic time until it has
installed an authoritative `ClockState`; LIVE/TESTNET remain unaffected.

Run:

```bash
bash validation/shared_clock_restart_resync_suite.sh
```

The suite first re-runs the STEP 35D source gate, the clock audit and an incremental build.
The first case forces a fresh runtime-image build by default so source and container binaries
cannot silently diverge. It then validates all seven REPLAY clock followers independently:

- market-data
- strategy
- portfolio-risk
- order-planner
- execution-state
- exchange-gateway
- simulated-exchange

For every case the suite:

1. starts a real distributed historical replay;
2. waits until economic cycles are already in flight;
3. pauses a different business-path service so the replay authority remains alive but the
   authoritative `ClockState` is held at one exact durable revision;
4. temporarily changes only the victim container's Docker restart policy to `no`, then injects
   `SIGKILL`; this prevents Compose `restart:on-failure` from creating an unobserved intermediate
   auto-restart while the harness is trying to delete/recreate the service;
5. proves the killed container stayed stopped (`RestartCount` unchanged), removes it, and recreates
   one final follower from the same Compose definition; the replacement must restore `on-failure`;
6. requires the final replacement to emit its own `clock_sync_requested` identity and accepts a
   replay-controller response only when `correlation_id` matches that exact replacement request;
7. the response or a same-revision refresh must contain the held `simulation_id`, `logical_time`
   and `revision`;
8. requires the recreated follower to log `clock_synchronized` with that exact state;
9. proves the authority did not advance merely to make the restart pass;
10. releases the causal blocker and requires replay to continue;
11. requires the final run to remain exactly `distributed == fast`;
12. rejects reconciliation blocks and clock identity/monotonicity failures.

This distinction matters because `restart:on-failure` and a forced `SIGKILL` can otherwise let a
short-lived process participate between the kill and `docker rm -f`. STEP 35D now proves recovery
for the container id that actually survives the recreation, not merely for any process that used
that service name during the injection window.

Expected final marker:

```text
STEP 35D CLOCK RESTART/RESYNC RESULT: PASS
Next: STEP 35E — replay-authority hard restart + durable clock recovery proof
```

To re-run only one failed follower case:

```bash
bash validation/shared_clock_restart_resync_suite.sh strategy
bash validation/shared_clock_restart_resync_suite.sh simulated-exchange
```

`STEP 35D` still does **not** close the full shared-clock phase. It proves follower recovery.
The next sub-step is **STEP 35E**: hard-restart the replay clock authority itself and prove that
its durable PostgreSQL checkpoint restores the exact active simulation clock without rollback,
skip, duplicate economic effects or divergence from the fast reference. Multiplier/MAX and
pause/resume economic equivalence remain subsequent clock-validation work.

## STEP 35E — Replay-authority hard restart + durable clock recovery proof

After the complete seven-follower `STEP 35D CLOCK RESTART/RESYNC RESULT: PASS`, validate the
other side of the shared clock: destroy the `replay-controller` authority itself while replay is
already in flight and prove that PostgreSQL, not process memory, is the source of recovery truth.

Run both causal phases:

```bash
bash validation/shared_clock_authority_restart_suite.sh
```

The suite validates two independent restart points:

- `decision`: Strategy is paused after `CLOSE(T)`, so the authority is held at a stable
  WAIT_DECISION barrier with no new durable DecisionBatch yet;
- `execution`: OrderPlanner is paused after Decision(T)/`OPEN(T+1)`, so one durable decision is
  ahead of the execution checkpoint and the authority is held in WAIT_EXECUTION.

For each phase STEP 35E disables only the controller victim's Docker restart policy before
`SIGKILL`, proves no intermediate auto-restart occurred, and then reads the clock/checkpoint rows
while **no replay-controller process exists**. The persisted `simulation_id`, `logical_time`,
`revision`, mode/speed/paused fields, message id, and decision/execution checkpoint shape must be
identical to the pre-crash state.

The controller is then recreated with the replay range recovered from PostgreSQL metadata. It must:

1. report `clock_recovered=true` with the exact held logical time/revision;
2. install the recovered value into its `SimulatedClock` and re-emit that same semantic state;
3. recover the exact decision/execution barrier phase;
4. leave PostgreSQL clock/checkpoint state unchanged while the causal blocker remains paused;
5. keep all seven clock followers alive with no simulation-id, rollback or same-revision conflict;
6. resume only after the blocker is released; and
7. finish with `DISTRIBUTED_FAST_COMPARE: PASS`.

Expected final marker:

```text
STEP 35E CLOCK AUTHORITY RESTART RESULT: PASS
Validated phases: decision execution
Next: STEP 35F — replay clock controls + speed/pause economic invariance
```

A single phase can be re-run while diagnosing a failure:

```bash
bash validation/shared_clock_authority_restart_suite.sh decision
bash validation/shared_clock_authority_restart_suite.sh execution
```

STEP 35E closes hard-restart recovery for both followers and the authority, but it still does not
close the complete fake-time phase. STEP 35F adds the REPLAY-only clock control plane and proves
that pause/resume and x1/x10/x60/x100/MAX pacing do not change economic outcomes. LIVE/TESTNET
remain on simple `SystemClock` and must not depend on those controls.

## STEP 35F — Replay clock controls + speed/pause economic invariance

After `STEP 35E CLOCK AUTHORITY RESTART RESULT: PASS`, activate the REPLAY-only operator
control plane for the shared logical clock and prove that presentation/pacing controls cannot
change trading economics.

Run:

```bash
bash validation/shared_clock_controls_suite.sh
```

STEP 35F adds `simulation.clock.control.v1` and the durable `ClockControl` contract. The
`replay-controller` is the only consumer. Supported actions are `Pause`, `Resume` and
`SetSpeed`; speed modes are `x1`/Realtime, positive multipliers such as x10/x60/x100, and
MAX/event-driven. A caller may send `expected_revision` as an optimistic-concurrency guard so a
stale dashboard cannot silently modify a newer replay state. Control changes increment the
logical-clock revision without moving `logical_time`, are committed to PostgreSQL before publish,
and are broadcast through the existing `ClockState` subject. The seven followers therefore
observe the same paused/speed state through the clock path they already validated in STEP 35D.

Pacing uses `steady_clock` only as wall-time measurement; it never becomes business time and it
is not a correctness barrier. `--clock-wall-scale` defaults to `1.0`, preserving literal
Realtime semantics (1 simulated second = 1 real second). The validation suite supplies a tiny
wall scale only to compress waiting during automated tests; this value is not stored in
`ClockState` and cannot change economic timestamps, decisions, orders, fills or PnL.

The suite proves all of the following:

1. LIVE/TESTNET `tradingRuntimeSubjects()` still excludes the fake-clock control subject;
2. control state uses COMMIT-before-publish and stale expected revisions are rejected;
3. x1, x10, x60, x100 and MAX each complete the same historical window with
   `DISTRIBUTED_FAST_COMPARE: PASS`;
4. a dynamic replay can be paused, remain on the exact same `logical_time/revision` for a real
   observation interval, change to x100 while still paused, and resume from that same time;
5. technical barrier timeout accumulation stops while operator pause is active; and
6. the pause/speed/resume run also finishes `distributed == fast exact`.

Expected final marker:

```text
STEP 35F CLOCK CONTROLS / ECONOMIC INVARIANCE RESULT: PASS
Validated modes : x1 x10 x60 x100 MAX
Next: STEP 35G — final shared-clock acceptance suite + LIVE isolation proof
```

STEP 35G is the closure gate for the whole shared-clock phase: re-run the audit/foundation,
follower restart, authority restart and control invariance evidence together, and add an explicit
LIVE/TESTNET isolation proof showing the trading runtime starts and operates with no fake-clock
subjects, consumers, simulation id or control-plane dependency.

## STEP 35G — Final shared-clock acceptance + LIVE/TESTNET isolation

After `STEP 35F CLOCK CONTROLS / ECONOMIC INVARIANCE RESULT: PASS`, close the entire
shared logical-clock phase with one acceptance orchestrator:

```bash
bash validation/shared_clock_acceptance_suite.sh full
```

A shorter diagnostic precheck is available:

```bash
bash validation/shared_clock_acceptance_suite.sh quick
```

`quick` is useful while diagnosing a regression, but **does not close the phase**. The
`full` scope is the release gate before dashboard implementation.

STEP 35G deliberately re-tests the current post-controls tree rather than merely trusting
older PASS logs. It performs:

1. source architecture and clock audit/foundation gates;
2. a real distributed follower synchronization replay;
3. a current-tree hard restart of the historically hardest follower (`market-data`);
4. both replay-authority hard-restart phases (`decision` and `execution`);
5. x1/x10/x60/x100/MAX plus dynamic pause -> speed-change -> resume invariance;
6. explicit LIVE **and** TESTNET runtime isolation with all seven services started without
   `--simulation-id`; their JetStream stream must contain no `simulation.clock.*` subjects
   and no `*-clock-state` durable consumers;
7. the complete pre-dashboard restart + chaos suite again after clock integration; and
8. the complete full-history EqualWeight + VolTarget economic/RealTest regression on the
   exact final tree.

The LIVE/Testnet isolation proof uses `validation/live_clock_isolation.override.yml` only as
a validation harness. It starts the seven existing runtime executables in production modes but
never starts `replay-controller`. Each service must advertise `runtime_mode=LIVE` or
`runtime_mode=TESTNET`, must have no simulation id in its process command, and must emit no
REPLAY clock-bootstrap markers. NATS monitoring is then inspected to prove that the production
runtime stream itself has no fake-clock subjects or durable fake-clock consumers.

The only closure marker is:

```text
STEP 35G SHARED CLOCK ACCEPTANCE RESULT: PASS
Shared logical clock phase : CLOSED
Next                       : STEP 36 — Trading Control Dashboard
```

Do not start dashboard implementation from a `quick` PASS or from an individual 35A-35F
sub-suite PASS. STEP 35G `full` is the integrated acceptance gate.
### STEP 35G FIX3 — LIVE/Testnet isolation fixture

`live_clock_isolation_suite.sh` intentionally uses a tiny valid OHLCV CSV for the LIVE/Testnet clock-isolation probe. The market-data service loads its full historical source before `event=service_ready`; using the normal multi-year dataset makes a clock-isolation check depend on host/bind-mount CSV parsing latency. The probe still requires every service to reach `event=service_ready`, remain running, advertise the requested runtime mode, omit `--simulation-id`, emit no REPLAY clock events, and leave JetStream free of fake-clock subjects/consumers. Override the fixture only with `CLOCK_ISOLATION_HISTORICAL_DATA=/absolute/path.csv`; readiness remains bounded by `CLOCK_ISOLATION_READY_TIMEOUT_SECONDS` (default 120 seconds) and writes service logs/inspect diagnostics on failure.

