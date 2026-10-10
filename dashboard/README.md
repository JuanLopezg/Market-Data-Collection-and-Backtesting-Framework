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

For an isolated five-minute CPU/RAM study with a 100-day canonical replay,
the service sandbox and dashboard included, run from the repository root under WSL:

```bash
python3 research/replay.py resources --days 100 --seconds 300 --open
```

This builds before measurement, uses a separate loopback port, polls authenticated
read resources and stops its own project automatically. It produces an offline
HTML graph and keeps existing dashboard/LIVE state intact. The UI observes canonical
simulation state; background services use a separate synthetic fixture. See
[research/REPLAY.md](../research/REPLAY.md) for the workload and resource-scope limits.

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

For current public data with virtual funds, use the separate
[paper deployment](../deploy/paper_trading/README.md). Infrastructure reads a fresh
host snapshot for CPU/RAM/disk and project container measurements, without a Docker
socket mount. Stale readings become unavailable. Container health is separate from
trading readiness. Trading executable observations, daily PAPER decision/plan-date
freshness and host systemd NTP status are now exposed separately. Missing NTP support
remains UNKNOWN; clock offset is not measured. The normal real overlay below
does not configure this optional snapshot by default.

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

## Local Telegram setup

All Telegram notifications use compact importance, account/system, reason and
detail text. WARN remains WARN, CRITICAL displays URGENT, and resolutions display
INFO with a resolved reason. Optional alert account labels pass through to the sink;
infrastructure alerts identify their system/service rather than invent an account.
Internal notification/alert/correlation IDs and machine timestamps remain in journals
and receipts, not chat text. Operational details are capped at 600 characters;
daily portfolios retain room for holdings within the Telegram message limit.
See the [Kraken account monitor](../live_trading/README.md#kraken-multi-m-preparation)
for the once-per-UTC-day USD portfolio summary and previous-day snapshot semantics.
Use `python3 validation/telegram_message_preview.py --send` under WSL to explicitly
send three clearly labelled synthetic format examples; without --send it only writes
ignored preview fixtures. The program never displays or copies credential contents.

The PAPER notifier now accepts `DASHBOARD_NOTIFIER_SINK=TELEGRAM`; TEST_FILE
remains the default. Create a bot with [BotFather](https://core.telegram.org/bots#how-do-i-create-a-bot),
start a conversation with it, and obtain the destination chat ID through your bot's
Bot API updates. Keep the token/chat ID in ignored `storage/paper_trading/.env`:

```dotenv
DASHBOARD_NOTIFIER_SINK=TELEGRAM
DASHBOARD_TELEGRAM_BOT_TOKEN=
DASHBOARD_TELEGRAM_CHAT_ID=
DASHBOARD_NOTIFIER_MIN_SEVERITY=WARN
```

After filling those fields, build the notifier locally and explicitly send one setup
message from the repository root in WSL:

```bash
docker compose --env-file storage/paper_trading/.env -f deploy/paper_trading/docker-compose.yml build dashboard-alert-notifier
docker compose --env-file storage/paper_trading/.env -f deploy/paper_trading/docker-compose.yml run --rm --no-deps dashboard-alert-notifier --test-message
```

Check the message in your chat. Then start continuous alert delivery with the same
Compose command ending in `up -d --no-deps dashboard-alert-notifier`. This sends
active WARN/CRITICAL alerts at startup, subsequent openings/escalations, eligible
updates after the default five-minute cooldown and resolution of notified alerts.
It does not send resolved historical alerts. Changing sinks uses a separate default
Telegram journal so prior TEST_FILE decisions do not suppress Telegram delivery.
Continuous polling requires a successful watchdog heartbeat no older than 90 seconds,
no more than 30 seconds ahead, and no failed last sweep. A stopped/stale watchdog
leaves delivery waiting; an old alert journal is not treated as current evidence.
PAPER notifier healthchecks reject recorded errors even after an earlier success.

The currently enabled local notifier uses the validated image through ignored
`storage/paper_trading/telegram.override.yml`; trading services remain stopped.
Use that override alongside the base Compose file when recreating this notifier,
or build the current source before returning to the default image. To stop only it:

```bash
docker compose --env-file storage/paper_trading/.env -f deploy/paper_trading/docker-compose.yml stop dashboard-alert-notifier
```

The real two-message controlled recovery check passed. To deliberately repeat it:

```bash
python3 validation/telegram_continuous_test.py --send-real
```

This sends TEST ONLY OPENED/RESOLVED messages from an isolated watchdog-format
journal, disconnects/reconnects its own container, verifies stale-source recovery
and restart receipts, then removes its container/state volume. It does not inject
alerts into the normal watchdog or start trading. Results are in CURRENT_STATE.md.

Fresh host resource pressure, unsynchronized NTP, missing/stopped trading executables
and degraded daily PAPER progress feed the existing watchdog lifecycle. Unavailable
clock/process observations and quiet no-order success are not invented failures.
Telegram [retry_after](https://core.telegram.org/bots/api#responseparameters) delays
all sends from the sink while it runs; failed events remain eligible for retry.
Restart retains delivery receipts, but the in-memory rate-limit deadline resets.
Receipts prevent resending a confirmed delivery whose notifier decision checkpoint
was lost. A remote acceptance followed by a crash before receipt persistence can
still produce a duplicate; Telegram sendMessage has no caller idempotency key.
The production endpoint is fixed, redirects are refused, and transport/API errors
redact credentials. No token belongs in VITE settings, URLs printed to logs or Git.
The first real local setup message was accepted by Telegram and its receipt on the
phone was confirmed by the user. Controlled continuous recovery also passed;
see CURRENT_STATE.md. This does not change the VPS.

## Current behavior and limits

- Authenticated local PAPER browser acceptance covers all eleven pages, login/logout,
  Pipeline interactions, Risk provenance, viewer restrictions and resource error/retry.
  The SSE hook closes its connection on full-document pagehide and reconnects on
  pageshow, preventing old documents from retaining connections that block REST reads.
  Operator routing, broader backend fault recovery and VPS acceptance remain
  separate. Evidence belongs in CURRENT_STATE.md.
- Overview charts show source-provided equity/reference values in USD, numeric
  Y-axis ticks, dates, hover readings, independent X/Y zoom and two-axis drag.
  Reset restores the selected window. Chart filters 1D/7D/30D/90D/1Y/ALL end at
  each series' latest observation, using UTC dates (1Y is 365 days). ALL means all
  received history, not an unbounded server query. One observation renders as a
  point; missing history remains unavailable. Filters affect charts only, not
  current snapshot statistics, positions or events. These changes are local;
  the running VPS retains its previous dashboard until a later authorized release.
- Pipeline joins Strategy, Risk and Planner checkpoints on the same economic timestamp.
  Quiet/HOLD cycles keep observed asset evidence and an explicit empty explanation.
  Missing decision/planner checkpoints stay pending; database failures remain errors.
  Missing signal entries are UNKNOWN; omitted targets mean HOLD, not liquidation.
  Replay shows market/account/retained-order observations, not service decision lineage
  or proof that cumulative historical fills belong to the current cycle.
- Positions/overview use timestamped canonical prices. Unpersisted cost basis, attributed
  PnL and historical equity remain unavailable.
- The ledger deterministically projects append-only fills into cash/asset deltas.
  It is not the runtime-owned accounting ledger still needed for complete PnL.
- Risk exposes the approved decision and persisted inputs. New local service cycles
  also persist evaluated sizing, volatility, asset/gross caps and reduction reasons
  in the risk checkpoint, with a cycle configuration fingerprint. HOLD preserves held
  quantity; evaluated weights do not become emitted targets. Old rows/replay snapshots
  retain unavailable intermediates. Whole-account/venue breaches remain unavailable.
  Persisted service identity
  supplies strategy allocation, sizer settings, gross/asset caps and rebalance policy;
  settings do not prove a breach evaluation. Combined display weights use total
  approved notional / combined emitted reference capital, not a sum of percentages
  with different denominators. Empty decision instructions do not mean zero exposure.
  Replay Risk displays marked account positions and explicitly unavailable policy/
  decision diagnostics; it does not infer an armed kill switch or a breach-free state.
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

The [project roadmap](../docs/ROADMAP.md) records local Overview chart/timeframe
acceptance, Pipeline/Risk investigations, Infrastructure completeness/readability,
perpetual funding/collateral accounting, Telegram setup, Manual Control execution
testing and Live vs. Expected baseline acceptance. Existing Hyperliquid public
integration is current implementation, not the initial live-venue priority;
Kraken is preferred subject to the roadmap's coverage and BTC-collateral checks.

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

Mobile dashboard access was cancelled by the user on 2026-10-09 and removed from
pending requirements. The unused bridge/tests remain optional reference work.
Local shadow-trading fixture preparation is implemented and passed under the
no-real-orders boundary. Actual selected-venue observation and aligned comparison
remain pending; see [the roadmap](../docs/ROADMAP.md#shadow-trading-preparation-and-acceptance).

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
