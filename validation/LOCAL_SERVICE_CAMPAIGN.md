# Controlled local service campaign

Use this before renting a VPS. It prepares a separate Docker project from the
current historical service topology, with an internal network, project-owned
NATS/PostgreSQL/SQLite volumes, random loopback ports and generated test credentials.
It never reads `deploy/live/.env`, mounts LIVE state or connects to a private venue.
Generated configuration, credentials and evidence remain under ignored
`storage/local_service_campaign/`. Do not commit them.

The current service chain includes the historical writer, strategy, risk,
execution-state, planner, gateway and simulated backend. The backend supplies
reconciliation and fills for this sandbox. This is separate from default LIVE,
which has no connected private execution, and from canonical RealTest parity.
Synthetic performance is not a strategy backtest result or a live-readiness claim.

## Prepare and smoke-test

Run from the repository root under WSL, with Docker running. Dependencies are the
current Meson build dependencies, Python 3/PyYAML, Docker Compose and installed
libnats (used for duplicate injection). Infrastructure uses `nats:2.10-alpine`,
`postgres:16-alpine` and `alpine:3.20`; the runtime image uses the existing Dockerfile.
Preparation compiles/packages current binaries and builds a separate image tag.
It leaves existing LIVE/historical image tags and all existing state intact.
After a successful current build, additional fresh fault scenarios can use
`prepare --skip-build` to reuse those binaries; packaging still checks all artifacts.
Do not use that option after compiled sources or build settings change. Some
Windows-mounted Ninja dependency caches can force unnecessary recompilation;
prefer a Linux-filesystem build for future sustained campaigns rather than silently
ignoring a build failure.
Set `BUILD_DIR` to that already configured Meson directory; both compilation and
runtime packaging use it.

```bash
python3 validation/local_service_campaign.py prepare
python3 validation/local_service_campaign.py start
python3 validation/local_service_campaign.py check
```

Preparation prints its run directory. Subsequent commands default to the last
prepared sandbox; add `--run storage/local_service_campaign/algotrading-local-...`
to operate an earlier one. Use explicit paths when running multiple campaigns.
Generated files have recorded hashes; editing configuration requires a fresh run.
Keep the one shared time file unchanged across service restarts.

The bounded fixture has 111 synthetic daily BTC/ETH bars. Its shared business
reference is 2020-04-20 noon at speed 1: April 19 is the only economic bootstrap
close, earlier bars provide warmup and April 20 provides only the execution open.
The check requires actual simulated fills, aligned durable strategy/risk/planner
checkpoints, unchanged reference-close maps, matching execution/backend cash and
positions, processed fill IDs and a drained backend outbox. It saves the settled
baseline, then compares later recoveries against it. Run within a few hours of
preparation; a later business-day boundary requires a fresh fixture.

On a failed command, capture evidence before changing anything:

```bash
python3 validation/local_service_campaign.py capture
```

Inspect `operations.log`, the daily service log files and durable evidence. A
failure stays a failure; do not update/delete `baseline.json` to accept drift.
Test state is retained for inspection until explicit cleanup.

## Duplicates and recovery

```bash
python3 validation/local_service_campaign.py duplicate
python3 validation/local_service_campaign.py fault --service strategy --operation kill
python3 validation/local_service_campaign.py fault --service strategy --operation start
python3 validation/local_service_campaign.py check
python3 validation/local_service_campaign.py fault --service execution-state --operation recreate
python3 validation/local_service_campaign.py check
```

Duplicate injection republishes recorded update, intent, decision, request, plan
and fill payloads without the broker's duplicate-suppression header. This reaches
service-owned idempotency rather than relying only on JetStream publication dedupe.
The disposable publisher runs inside the internal Docker network; injection does
not require host TCP access to NATS.
Repeat kill/start and recreation for risk, planner, gateway and simulated backend.
The feeder may be recreated too; its SQLite checkpoint recovery republishes the
last committed update while preserving visibility boundaries.

To exercise infrastructure loss:

```bash
python3 validation/local_service_campaign.py fault --service nats --operation kill
python3 validation/local_service_campaign.py fault --service postgres --operation kill
python3 validation/local_service_campaign.py fault --service nats --operation start
python3 validation/local_service_campaign.py fault --service postgres --operation start
```

Inspect evidence, then restart affected business services with `fault --operation
restart` and run `check`. Automatic reconnection is a separate observation: services
use their existing code and the sandbox disables Docker auto-restart so exits remain
visible. A successful manually assisted recovery does not prove unattended recovery.
Recreate NATS/PostgreSQL separately with `--operation recreate`; named volumes stay
attached. Do not remove volumes between a fault and its recovery check.

## Exact checkpoint and outbox barriers

Each checkpoint barrier needs a **fresh** prepared project:

```bash
python3 validation/local_service_campaign.py prepare
python3 validation/local_service_campaign.py start --hold-service strategy
python3 validation/local_service_campaign.py check
```

Repeat on fresh projects with `--hold-service portfolio-risk` and `--hold-service
order-planner`. The harness installs a test-only PostgreSQL trigger that blocks
the target checkpoint INSERT. Current handlers publish before this INSERT. It
observes the blocked backend, kills the owning service, terminates only that
blocked SQL backend, verifies checkpoint absence, removes the trigger and starts
the service for durable redelivery. `checkpoint_crash.json` records the boundary.
No production-code fault hooks or strategy behavior changes are needed.

On an accepted sandbox, run:

```bash
python3 validation/local_service_campaign.py outbox-crash
python3 validation/local_service_campaign.py duplicate
python3 validation/local_service_campaign.py check
```

This deliberately resets only test outbox publication receipts to model a lost
receipt, leaving account/order/fill truth intact. A second trigger blocks marking
the receipt after actual publication. The harness observes the block, crashes the
backend, removes the test trigger and verifies recovered publication with unchanged
economics. `outbox_crash.json` identifies the receipt-loss fixture. Failed barrier
runs may leave their trigger installed; preserve evidence and clean up that project
rather than reuse it. The separate adapter test covers commit-before-ACK failure:

```bash
python3 validation/transport_persistence_integration_test.py
```

## Daily logs and CPU/RAM

For the complete service sandbox plus dashboard and an original-data, 100-day
canonical backtest in a separate container, use the one-command
[resource campaign](../research/REPLAY.md#partial-history-days):

```bash
python3 research/replay.py resources --days 100 --seconds 300
```

It excludes setup from measurement, automatically stops its own stack and writes
an offline HTML CPU/RAM graph. The dashboard observes canonical simulation state;
the background services retain the distinct synthetic smoke fixture. Resource
profiles and full-history/VPS/real-trading capacity acceptance remain separate.
The manual `monitor` command below samples only its prepared Compose project;
it does not attach a dashboard automatically.

```bash
python3 validation/local_service_campaign.py monitor --seconds 60
python3 validation/local_service_campaign.py report
python3 validation/daily_service_logs_test.py
```

`resources.jsonl` records Docker CPU/RAM samples with UTC timestamps and container
attribution. `resource_summary.json` reports typical and sampled peak usage. Sampling
can miss short peaks; this small fixture does not establish full-backtest/live capacity.
Use a second WSL terminal to monitor while injecting faults. Capture typical idle,
startup, active processing and recovery periods separately when estimating VPS size.

Service stdout/stderr is copied incrementally into UTC daily files under the run's
`algotrading/services/` directory. Captured files append across container recreation,
have restricted Linux permissions and use the existing five-day cleanup policy.
Capture/check/monitor perform collection; Docker caches stay size-bounded between
collections. Monitoring collects logs roughly every minute. This is a local collector,
not installation or acceptance of the VPS rsyslog/systemd receiver. The focused logging
test verifies receiver restart, UTC templates, scope, expiry and redaction independently.
A real midnight/quiet-service scheduled-expiry test still needs elapsed-time evidence.

For a bounded baseline backtest CPU/RAM measurement, use the existing public CLI:

```bash
/usr/bin/time -v -o storage/local_backtest_resources.txt \
  python3 research/replay.py fast --days 107 --label local_resource_baseline \
  > storage/local_backtest_resources.log 2>&1
```

Before a full-history capacity study, explicitly choose the source/window and retain
duration, peak RSS, CPU and host/WSL limits. Full-history runs are intentionally not
part of setup or the default sandbox smoke check. Real exchange funding, partial-fill
realism and live CPU/RAM remain separate future acceptance work.

## Cleanup and acceptance

To pause the prepared environment while preserving its volumes and evidence:

```bash
python3 validation/local_service_campaign.py stop
python3 validation/local_service_campaign.py start
```

Resume within the fixture's time window; otherwise prepare a fresh project.
For permanent removal of its test state:

```bash
python3 validation/local_service_campaign.py cleanup
bash validation/step59_canonical_replay_release_gate.sh
```

Cleanup captures diagnostics and removes only this generated project's containers,
network and volumes. Evidence and the built validation image remain available.
Use the same explicit `--run` path when cleaning older projects. Never run LIVE
`down -v` as a substitute. No command rents or deploys a VPS, sends a real order,
commits or pushes changes.

Record each observed case in `CURRENT_STATE.md`, including failures, interventions,
resource limits and pending cases. The required Step59 fingerprint remains
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`.
Accept local preparation only after a bounded smoke run; accept the complete local
campaign only after all chosen fault/recreation/log/resource cases pass. VPS hardware,
host logging, deployment and private venue behavior still require later verification.
