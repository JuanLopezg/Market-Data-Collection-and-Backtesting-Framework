# Step 43 — Durable Alerts / Watchdog Foundation

Status: implementation package prepared; do not mark PASS until `scripts/step43-alert-watchdog.sh` passes on the real local stack.

## Scope

Step 43 adds a dashboard-independent watchdog process to the observability stack. It derives current operational alerts from the same read-only canonical sources as `dashboard-api` and persists only alert lifecycle transitions into a dedicated append-only observability store.

The watchdog does **not** write trading PostgreSQL, publish trading commands, sign exchange requests, submit/cancel orders, or require a wallet/funds.

## Durable lifecycle store

Dedicated named volume: `dashboard-watchdog-data`.

Files:
- `/data/watchdog/events.jsonl` — append-only lifecycle transitions (`OPENED`, `UPDATED`, `RESOLVED`).
- `/data/watchdog/status.json` — atomically replaced watchdog heartbeat/status document.

The store is intentionally separate from runtime trading persistence. Trading remains independent if the watchdog/dashboard is unavailable.

## Alert sources

Current derived alerts include existing infrastructure, reconciliation, execution rejects, market-data, risk and Step 37A symbol-registry conditions. Step 43 adds Step 42 ledger-integrity conditions:
- transient PostgreSQL ledger read failure while routing is disabled -> `WARN`;
- ledger identity/economic integrity contradiction -> `CRITICAL`.

The durable store records lifecycle transitions for the resulting current alerts. It does not manufacture historical state before the watchdog was started.

## API/UI

`GET /api/alerts-audit` now also exposes:
- `watchdogAvailable`
- `watchdogState`
- `watchdogLastSweepAt`
- `watchdogLastSuccessAt`
- `durableEventCount`
- `durableLifecycleEvents`
- real `resolved24h` derived from durable `RESOLVED` transitions in the bounded lifecycle window.

The Alerts & Audit page shows watchdog state and durable lifecycle evidence.

## Deliberately still unavailable

- Alert acknowledgement mutation/state.
- Durable human-action audit ledger.
- Telegram notification delivery.
- Private Hyperliquid auth/signing.
- Submit/cancel/order routing.

Those are not faked. `acknowledgementAvailable=false` and `humanAuditAvailable=false` remain explicit.

## Safety

- Watchdog exposes no port.
- Watchdog root filesystem is read-only.
- Only its own `/data/watchdog` volume is writable for lifecycle evidence.
- Canonical market-data DB file is mounted read-only.
- PostgreSQL integration remains SELECT-only.
- Manual route remains disabled.
- Step 37 private auth and Steps 38–41 remain deferred.

## Gate

Run after rebuilding the dashboard stack:

```bash
./scripts/step43-alert-watchdog.sh
```

Expected final line:

```text
STEP 43: PASS — DURABLE ALERTS / WATCHDOG FOUNDATION VALIDATED
```

Next planned safe step without wallet: Step 44 — Full Global Readiness Contract.
