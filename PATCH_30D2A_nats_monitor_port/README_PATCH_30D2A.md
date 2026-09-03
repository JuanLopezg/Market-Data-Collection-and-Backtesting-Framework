# PATCH 30D2A — independent NATS monitoring port for parallel diagnostic topologies

## Why

PATCH 30D2 did not reach the replay. Docker failed because the previous preserved STEP 30D topology was still using host port `58228` for the NATS monitoring endpoint (`8222`).

The historical harness already allowed a custom NATS client port, but it did not expose `REPLAY_NATS_MONITOR_PORT`, so two independent Compose projects could still collide on the monitoring port.

This patch changes validation/harness plumbing only. No strategy, risk, order-planning, execution, exchange or persistence semantics change.

## SUSTITUIR

- `tools/distributed_compare/run_historical_compare.sh`
- `validation_30D2/run_validation.sh`

## AÑADIR

- `README_PATCH_30D2A.md`

## ELIMINAR

Nada.

## New behavior

`run_historical_compare.sh` accepts:

```text
--nats-monitor-port N
```

If omitted, it derives the monitoring port as:

```text
nats client port + 4000
```

Examples:

```text
54230 -> 58230
54234 -> 58234
```

PATCH 30D2 validation now explicitly uses:

```text
NATS client     54234
NATS monitoring 58234
PostgreSQL      55444
```

so the preserved STEP 30D topology can remain alive for diagnostics.

## Run

From project root, after copying/replacing the files:

```bash
bash validation_30D2/run_validation.sh
```

The harness automatically tears down any stale `algotrading_step30d2_validation` project before starting, so the partially-created failed 30D2 topology does not need manual cleanup.

Do **not** destroy the preserved `algotrading_step30d_validation` topology yet unless you no longer need its JetStream/PostgreSQL state.
