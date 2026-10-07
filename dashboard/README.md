# Control Dashboard

The React UI and Go API observe trading through bounded read models. Trading must
continue if the dashboard stops. The browser uses authenticated HTTP/SSE; it never
connects directly to a database, NATS or an exchange.

The dashboard supports real reads, canonical replay observation, durable watchdog alerts,
operator acknowledgements, notification adapters and audited manual-intent admission.
Actual private exchange submission remains disabled.

## Choose the data source

| API provider | Source and meaning |
| --- | --- |
| `mock` | Embedded fixtures for UI development; not trading evidence. |
| `real` | PostgreSQL runtime/checkpoints/fills, canonical market SQLite, retained NATS evidence and separate observability stores. Missing evidence stays unavailable or blocked. |
| `simulation` | State published by the canonical replay runner. Same replay economics as system mode. |

Provider identity is visible in the UI and responses. Real mode never silently falls
back to fixtures. Process health is separate from source reachability, observational
readiness and trading readiness. The real provider intentionally does not claim full
readiness while private trading/control/accounting boundaries remain incomplete.

## Local UI development

From `dashboard/`:

```bash
docker compose up --build -d
```

Open `http://localhost:8080`. Local demo credentials are
`viewer / viewer-demo` and `operator / operator-demo`.
Production disables demo credentials.

For frontend development/build:

```bash
npm ci
npm run dev
npm run build
```

The frontend's data-source interfaces are independent of database schemas and C++
service internals. `VITE_*` settings are public bundle data; never put secrets there.

## Observe canonical replay

From the repository root:

```bash
python3 research/replay.py dashboard --days 107 --label dashboard_107d
```

The public wrapper runs the system simulation, publishes
`deploy/historical_replay/run/step58_dashboard/state.json` and starts the
simulation dashboard unless `--no-dashboard-up` is supplied.

`--start` starts strategy state cold. To observe a later interval with mature
history, replay from the normal beginning and use `--pace-start`/`--pace-end`.
Pacing and UI publication do not change economics. Checkpoint/resume controls are shared
with system mode. See [research/REPLAY.md](../research/REPLAY.md).

The compact release gate checks simulation equality/restart/pacing without a browser.
The slow paced browser campaign is separate acceptance work, not implied by that gate.

## Observe the running live stack

Start the trading stack described in [deploy/live/README.md](../deploy/live/README.md).
Then, from the repository root:

```bash
docker compose --env-file deploy/live/.env \
  -f dashboard/docker-compose.yml \
  -f dashboard/docker-compose.real.yml up --build -d
```

The real overlay expects the existing `algotrading-live_live` network and
`storage/databases/database.db`. It obtains PostgreSQL settings from the live
environment file. Keep credentials out of commands/logs and use a SELECT-only database
role where available.

The canonical SQLite file is over-mounted read-only. Its directory remains writable
for SQLite WAL locking/sidecar bookkeeping; API queries use `-readonly` and
`PRAGMA query_only=ON`. This is not permission to modify market history.
The watchdog has its own durable store, and API manual-audit/acknowledgement stores
are separate from trading data.

This overlay configures the Hyperliquid TESTNET public/dry-run boundary. Public metadata,
symbol mapping and venue rules do not prove private authentication or execution.
Changing provider mode does not enable private orders.

## Current behavior and limits

- Pipeline joins Strategy, Risk and Planner checkpoints on the same economic timestamp.
  Quiet/incomplete cycles stay pending; contradictions block.
- Positions/overview use timestamped canonical prices. Unpersisted cost basis, attributed
  PnL and historical equity remain unavailable.
- The ledger deterministically projects append-only fills into cash/asset deltas.
  It is not the runtime-owned accounting ledger still needed for complete PnL.
- Risk exposes the approved decision and persisted inputs. Raw sizing, binding constraints
  and intermediate transforms are not invented by the UI.
- Live-vs-Expected projects market/strategy-input anomalies. Execution/accounting and
  accepted-replay baseline families remain deferred.
- Watchdog alert lifecycle, operator acknowledgements and notifier receipts are durable.
  Acknowledgement does not resolve an alert or change trading readiness.
- Manual preview validates CSV/targets against current reference state. Route admission
  binds the request/reference and durably audits operator intent, always with
  `submitted=false` and routing disabled.
- Private authentication, the trading-control sink and real submit/cancel/fill lifecycle
  remain deferred. No dashboard page bypasses normal trading risk/execution ownership.

The integration catalog still includes historical planning metadata for some resources.
Use current provider handlers, resource responses and readiness contracts to determine
implemented capabilities; do not treat a catalog label as execution authorization.

## Documentation and source navigation

| Guide | Purpose |
| --- | --- |
| [API_CONTRACT.md](docs/API_CONTRACT.md) | Current routes, response/auth rules and capability limits. |
| [REAL_DATA_SOURCE_MAP.md](docs/REAL_DATA_SOURCE_MAP.md) | Actual source ownership, schemas, joins and accounting gaps. |
| [AUTH_SECURITY.md](docs/AUTH_SECURITY.md) | Sessions, roles, CSRF and credential boundary. |
| [AWS_DEPLOYMENT.md](docs/AWS_DEPLOYMENT.md) | VPS/production network, configuration and startup. |
| [PRODUCTION_CHECKLIST.md](docs/PRODUCTION_CHECKLIST.md) | Concrete deployment checks. |

Start in `dashboard-api/internal/server/server.go` for routes/auth/actions,
`dashboard-api/internal/provider/` for resource builders and
`dashboard-api/internal/integration/` for bounded source reads.
The frontend lives in `src/`. Deployments and scripts remain at the dashboard root.

## Validation

From the repository root under WSL:

```bash
python3 dashboard/scripts/verify-project-contracts.py --project-root .
bash validation/step59_canonical_replay_release_gate.sh
```

Backend tests, from `dashboard/dashboard-api/`:

```bash
go test ./...
```

`scripts/step*.sh` retain focused integration/security/observability gates and
accepted step identities. Many require a running dashboard and specific real fixtures;
read their prerequisites before running them. They are not all replaced by the source
marker audit. Production preflight/smoke commands are in the deployment guide.
