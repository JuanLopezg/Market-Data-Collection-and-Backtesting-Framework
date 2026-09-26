# Control Dashboard — Step 45

Lightweight desktop-first algo-trading operations dashboard with a Grafana-like visual direction.

This archive is cumulative through Step 45.

## What Step 14 changes

Step 14 is the first step grounded in the uploaded **real algoTrading codebase**. It does not connect to production data yet; instead it replaces the old unverified source worksheet with an audited source map and machine-readable integration catalog.

```text
Browser
   │
   ▼
React SPA
   │ /api/*
   ▼
Dashboard API
   │
   ▼
provider.Provider
   ├── EmbeddedMock        ← working mode
   └── Real                ← bounded read-only adapters enabled resource-by-resource
          │
          ├── PostgreSQL   ← audited tables/snapshot fields
          ├── SQLite       ← audited canonical market-data schema
          └── NATS         ← audited subjects/wire contracts
```

## Verified against the project

Step 14 reviewed the actual runtime sources and verified:

- canonical NATS subjects in `lib/src/transport/transport_subjects.h`;
- JSON transport fields in `lib/src/transport/contract_json_codec.cpp`;
- ExecutionState PostgreSQL tables/snapshot structure;
- Strategy, PortfolioRisk and OrderPlanner checkpoint tables;
- canonical market-data SQLite schema;
- service ownership and current live Docker topology;
- the current pre-exchange status of the live deployment.

See:

- `REAL_DATA_SOURCE_MAP.md`
- `RUNTIME_CONTRACT_AUDIT.md`
- `INTEGRATION_GAPS.md`
- `STEP_14_NOTES.md`

## New audit endpoint

Authenticated users can query:

```text
GET /api/integration-catalog
```

It returns the Step 14 resource map with statuses such as:

```text
VERIFIED_BASE
PARTIAL
MISSING_PROJECTION
NOT_IMPLEMENTED
```

This is diagnostic metadata; it does not make `RealProvider` ready.

## Real provider remains fail-closed

```text
DASHBOARD_DATA_PROVIDER=real
```

Step 14 established the mapping. Steps 15–20 then enabled bounded read-only adapters incrementally. In real mode, wired resources are served from canonical sources while unwired resources still fail closed with no fallback to mock data.

## Current implemented product

- React + TypeScript + Vite static SPA.
- Overview, Positions, Reconciliation, Pipeline + Why?, Execution, Risk, Market Data, Infrastructure, Alerts & Audit, Live vs Expected, Manual Control preview.
- Step 31: Pipeline includes a read-only end-to-end evidence proof from aligned Market/Strategy/Risk/Planner checkpoints through runtime order, durable fill and reconciliation state.
- Step 32: authenticated pre-testnet safety gate separates safe-to-continue integration from strict trading readiness and adds a fail-closed CLI acceptance audit.
- Step 33: explicit Hyperliquid TESTNET integration foundation; venue identity is visible while trading remains disabled.
- Step 34: bounded public Hyperliquid TESTNET connectivity/metadata probe (`meta` + `allMids`) with no credentials, private account calls, signing or order routing.
- Step 44: explicit phase-scoped global readiness contract separates safe dashboard continuation from private-testnet/trading/LIVE authorization.
- Step 45: versioned/fingerprinted Live vs Expected REAL projection with explicit coverage boundaries and no expected-PnL claim.
- Step 45: Live vs Expected is a REAL versioned/fingerprinted read-only anomaly projection over canonical market/strategy-input history; execution, accounting/PnL and accepted-replay historical distributions remain explicit DEFERRED coverage rather than being fabricated.
- Frontend mock/API data-source abstraction.
- Go dashboard API with authentication and all current read endpoints.
- VIEWER / OPERATOR roles, HttpOnly session cookies and CSRF plumbing.
- Caddy reverse proxy and HTTPS-ready production deployment.
- Docker private network, resource caps, read-only containers and security headers.
- Server-side provider abstraction.
- Audited PostgreSQL / SQLite / NATS integration boundaries.
- Visible data-provider identity badge so mock data cannot be confused with live data.

## Local Docker run

```bash
docker compose up --build -d
```

Open `http://localhost:8080`.

Demo credentials are local-only:

```text
viewer   / viewer-demo
operator / operator-demo
```

## Optional: verify this mapping against a project checkout

```bash
python3 scripts/verify-project-contracts.py --project-root /path/to/algoTrading
```

This script checks only audited source markers and does not read `.env` files or secrets.

## Production-shaped run

```bash
cp .env.production.example .env.production
# Set domain and strong passwords.
# Keep DASHBOARD_DATA_PROVIDER=mock until real adapters are implemented.

./scripts/production-preflight.sh .env.production

docker compose \
  --env-file .env.production \
  -f docker-compose.production.yml \
  up --build -d
```

## Current integration direction

The dashboard is now incrementally enabling verified read-only resources. Infrastructure is real in Step 16 and Positions becomes real in Step 17. Unsupported resources continue to fail closed in `DASHBOARD_DATA_PROVIDER=real`; no page silently falls back to mocks.

No C++ trading semantics are modified merely to make the dashboard easier to implement.


## Step 15

Read-only real-source reachability foundation added. See `STEP_15_NOTES.md` and `REAL_SOURCE_QUICKSTART.md`. Default local compose remains mock-safe.

## Step 16

The first normal page can now run on real sources: **Infrastructure**. PostgreSQL is queried with fixed read-only SQL through the official `psql` client, NATS monitoring is read from `/jsz` + `/varz`, and the canonical SQLite market-data file is probed read-only. Unknown host/container/exchange/reconciliation facts remain explicitly UNKNOWN rather than being mocked. See `STEP_16_NOTES.md` and `REAL_SOURCE_QUICKSTART.md`.


## Step 17

**Positions** is now the second normal page enabled in real-provider mode. It reads the durable `trading_runtime_state.snapshot` through a fixed bounded PostgreSQL query and decodes the verified runtime contract. The page exposes real physical account quantities plus per-strategy virtual-position breakdowns.

Step 17 deliberately does **not** invent entry price, mark price, PnL, current weight, approved target or exchange-reconciliation state. Those fields render as unavailable until their canonical sources are wired. With the currently observed runtime snapshot (`positionCount: 0`) the real Positions page correctly renders an empty portfolio and the durable account cash value.

Step 18 completed this recommendation with a bounded durable Execution read model.


## Step 18 update
`Execution` now has a bounded real read model from `trading_runtime_state.snapshot.orders` + `trading_fills`. Latency/slippage/replacement lineage remain unavailable unless a canonical durable source is added.


## Step 19 update

**Reconciliation** is now enabled in real-provider mode. The API reads the durable local `trading_runtime_state.snapshot` and performs a read-only JetStream direct-get of the latest retained `execution.exchange.snapshot.v1` event from `ALGOTRADING_RUNTIME`. It mirrors the verified C++ `Reconciler` comparisons for cash, positions, and open orders using the runtime defaults (`1e-6` cash, `1e-10` quantity).

Safety rule: the dashboard never publishes `execution.exchange.snapshot.request.v1`. If exchange evidence is missing or older than the current PostgreSQL runtime snapshot, the page reports `PENDING` and does not promote stale evidence to `CLEAN` or a current blocking verdict. Fresh mismatches are surfaced as `BLOCKED` because the runtime reconciler treats those discrepancies as blocking during recovery.

Target quantity/value remains unavailable until the durable planner/checkpoint lineage is wired.

Step 20 completes that recommendation: **Pipeline / Why** now reconstructs one cycle-aligned durable lineage from Strategy + PortfolioRisk + OrderPlanner + ExecutionState. Non-persisted RSI/rank/intermediate risk transforms are shown as unavailable rather than inferred.


## Step 20 update

**Pipeline / Why** is enabled in real-provider mode. The API anchors on the latest `portfolio_risk_live_decision_checkpoint` and joins Strategy and OrderPlanner checkpoints only at the exact same business timestamp, then enriches the trace with durable ExecutionState and reconciliation evidence. The UI exposes persisted signal intent, approved target notional, planner state revision, planned orders, durable execution state and reconciliation. RSI, liquidity rank, raw target and intermediate volatility/risk transforms remain explicitly `Not persisted`.


## Step 21 update

Risk now has a bounded read-only REAL projection from `portfolio_risk_live_decision_checkpoint`. Approved DecisionBatch targets and their exact persisted signal/account inputs are shown. Active config limits, intermediate risk transforms, breach reports and global readiness remain unavailable rather than inferred.


## Step 22 update

Market Data is now a real read-only resource. The API reads the canonical SQLite ranking/OHLCV window, recomputes the current PureRSI SMA Volume(25) and RSI(7) diagnostics with the audited formulas, and joins durable StrategyIntent state only when the PostgreSQL strategy checkpoint is exactly cycle-aligned. See `STEP_22_NOTES.md`.


## Step 23 update

Overview is now a real read-only aggregate over the verified runtime, risk, execution, reconciliation and market-data read models. It fails closed on missing durable account state, never fabricates equity/PnL/history, and never promotes global readiness to READY before the dedicated shell/readiness step. See `STEP_23_NOTES.md`.


## Step 24 update

`/api/alerts-audit` is now enabled in real-provider mode as a **derived read-only operational projection**. It aggregates current critical/warning conditions from PostgreSQL/NATS source health, reconciliation evidence, durable execution rejects, canonical market-data diagnostics and PortfolioRisk/Strategy checkpoints.

The evidence table is intentionally labelled **System Evidence Timeline** rather than a complete human audit ledger. The project still has no append-only alert history, acknowledgement/resolution store, or durable actor audit table, so Step 24 returns explicit availability flags instead of inventing those records.

## Step 26 status

Manual Control Step 26 introduced the authenticated backend preview. Step 46 now supersedes its audit gap with a durable dashboard-owned operator-intent store and a confirmed/hash-bound/stale-reference-checked route-admission endpoint. Actual routing remains disabled until the trading-control/PortfolioRisk manual contract, private auth and order lifecycle are proven. See `STEP_26_NOTES.md` and `STEP_46_NOTES.md`.

## Step 27 update

The persistent application shell now has a real fail-closed aggregate from `/api/shell-status`. PostgreSQL/NATS, durable runtime-state presence, canonical market-data diagnostics, reconciliation evidence and derived active alerts are combined into one operator status bar. Fresh retained exchange evidence is not mislabeled as gateway connectivity, and `READY` remains withheld until exchange connectivity, common service liveness/trading-control state and host clock synchronization have verified sources. See `STEP_27_NOTES.md`.

## Step 28 — live updates without aggressive polling

Authenticated `/api/stream` SSE now bridges read-only NATS subject invalidations into the browser. React refetches only mounted REST read models affected by the event, with a 30-second fail-safe refresh. The browser never connects to NATS and the streaming bridge never publishes or mutates trading state. The top bar displays the current SSE connection state.

## Step 29 — resilience and performance hardening

Step 29 bounds dashboard resource reads and SSE fan-out, adds authenticated `/api/diagnostics`, shows dashboard-process metrics on Infrastructure, logs slow/5xx requests with status/duration, and coalesces frontend invalidation bursts. These protections are local to the dashboard and do not alter the trading pipeline.

## Step 30 — AWS / VPS production hardening

Step 30 adds the real-source production Compose overlay, fail-closed production preflight, HTTPS/auth/source/SSE smoke test, VPS/AWS deployment guidance and a reviewed systemd unit example. It does not enable any new trading action. See `STEP_30_NOTES.md` and `AWS_DEPLOYMENT.md`.


## Step 32 — pre-testnet safety gate

See `STEP_32_NOTES.md`. The new `/api/safety-gate` endpoint and Infrastructure panel remain read-only and explicitly do not authorize trading. Run `./scripts/step32-live-safety-gate.sh` against the local real stack before proceeding to any testnet adapter work.


## Step 33 — testnet integration foundation

Step 33 establishes the **configured execution-venue identity** without connecting to the venue. The current audited C++ `ExchangeGateway` already contains a `hyperliquid-dry-run` prepare-only mode, so the dashboard now exposes `HYPERLIQUID / TESTNET / hyperliquid-dry-run` as the intended next integration boundary.

Authenticated `GET /api/venue-foundation` and the Infrastructure panel explicitly report:

- public connectivity: `NOT_CHECKED`;
- private auth: `DISABLED`;
- order routing: `DISABLED`;
- symbol mapping: `NOT_CONFIGURED`;
- exchange filters: `NOT_LOADED`;
- secrets required: `false`;
- real capital required: `false`.

Run `./scripts/step33-testnet-foundation.sh` after starting the normal local real stack. Step 34 now performs a separate public-only connectivity/metadata check; the Step 33 foundation endpoint itself remains configuration evidence only.


## Step 34 — public Hyperliquid TESTNET connectivity / metadata

Step 34 introduces a dedicated read-only public venue adapter. `dashboard-api` makes only fixed `POST /info` requests to the pinned TESTNET endpoint:

- `{"type":"meta"}` for perpetual-universe metadata;
- `{"type":"allMids"}` for public mid prices.

Authenticated `GET /api/venue-public` returns a bounded summary: connectivity, universe/mid counts, overlap, sample symbols, latency and the exact public endpoint. The endpoint does **not** return or request account data.

Safety boundary remains:

- private auth: `DISABLED`;
- order routing: `DISABLED`;
- symbol mapping: `NOT_CONFIGURED`;
- venue trading rules/filters: `NOT_APPLIED`;
- secrets used: `false`;
- capital used: `false`;
- execution/private connectivity remains `UNKNOWN` in the existing Infrastructure exchange block.

Run `./scripts/step34-public-venue.sh` after starting the normal local real stack. A PASS proves only that Hyperliquid TESTNET public metadata is reachable and structurally usable. Step 35 may add explicit symbol-mapping validation; it still must not add private credentials or order submission.

## Step 35 — Explicit symbol mapping

The dashboard now exposes authenticated read-only `/api/venue-symbol-map` and explicitly classifies the current canonical strategy universe against Hyperliquid TESTNET. Exact mappings present in current public metadata are supported; explicitly unsupported or currently venue-absent assets remain non-routable. Any unclassified symbol still fails closed. Run `./scripts/step35-symbol-mapping.sh` after the normal local real-stack startup. This step does not enable private auth, exchange filters or order routing.

## Step 36 — Public venue trading rules / precision

Step 36 adds authenticated read-only `GET /api/venue-rules` and an Infrastructure panel that validates public Hyperliquid TESTNET trading constraints only for the Step 35-supported mappings. It reads public `meta` + `allMids`, exposes `szDecimals`, derived size step, price precision constraints, `maxLeverage`, margin-mode flags, delisted state, current mid and a diagnostic minimum-size estimate for the documented 10 USD minimum order notional. Step 35-unsupported assets remain non-routable. No private auth, signing, submit/cancel or capital path is added.

Run `./scripts/step36-venue-rules.sh` after the normal local real-stack startup. Step 37 must not begin until the runtime gate reports PASS.

## Step 37A — Multi-exchange symbol registry + drift alarms

Step 37 private wallet authentication is currently deferred. Step 37A adds a versioned, explicit cross-venue registry at `dashboard-api/internal/integration/symbolregistry/registry.json` plus authenticated read-only `GET /api/symbol-registry`. The registry records exact Binance/source symbols and exact execution-venue symbols/classifications and is intentionally extensible to future exchanges. Runtime suffix stripping, fuzzy matching and automatic aliases are forbidden.

The registry is cross-checked against the accepted Step 35 manifest, the current canonical market-data ranking, the current strategy universe and current Hyperliquid TESTNET metadata. New/unregistered symbols, registry divergence and venue disappearance generate fail-closed diagnostics; partial execution coverage generates WARN alarms without making blocked assets routable. These alarms are also surfaced in Alerts & Audit as derived operational evidence.

Run `./scripts/step37a-symbol-registry.sh`. This step requires no wallet, no private key, no signing, no order route and no funds.

## Step 37A.2
A transient read-only dependency failure in the SymbolRegistry is WARN while order routing is disabled, preventing a circular Step32 prerequisite. Registry contradictions, unregistered assets and artifact/manifest failures remain CRITICAL; the Step37A gate itself still requires a validated registry.


### Step 37A.3 live registry refresh

The registry is versioned and intentionally does not auto-learn execution aliases. If the canonical Binance/source ranking introduces a new symbol, the gate raises a CRITICAL unregistered-symbol alarm. Step 37A.3 classifies the nine symbols first observed by the live gate on 2026-09-26 as explicit non-routable entries pending manual venue mapping review.

### Step 42 — append-only ledger foundation

The authenticated dashboard now exposes `GET /api/ledger`, a deterministic read-only economic projection over runtime-owned append-only `trading_fills`. It validates unique fill identities/basic economics, surfaces all-time fee/buy/sell aggregates and a bounded recent fill-derived ledger window in Execution. This is a foundation only: durable cost basis, realized/unrealized PnL attribution and historical equity remain explicitly unavailable until owned by a canonical runtime accounting projection. Private Step 37 and Steps 38-41 remain deferred; Step 42 needs no wallet or funds.

## Step 43 — durable alerts/watchdog

Step 43 adds a separate `dashboard-watchdog` process in the real-data compose overlay. It derives alerts read-only from canonical sources and writes only an isolated append-only observability lifecycle store (`OPENED` / `UPDATED` / `RESOLVED`) in the `dashboard-watchdog-data` volume. `dashboard-api` mounts that store read-only and exposes the lifecycle evidence through `GET /api/alerts-audit`. It does not add acknowledgement mutations, Telegram delivery, wallet/signing, or order routing. See `STEP_43_NOTES.md` and `scripts/step43-alert-watchdog.sh`.

## Step 44 — Full Global Readiness Contract

Step 44 adds `GET /api/global-readiness`, an authenticated GET-only aggregate that makes readiness phase-scoped and explicit. A validated contract may allow continued dashboard development while `privateTestnetReady`, `tradingReady` and `liveReady` remain false. This avoids treating a generic green status as permission to trade.

The contract includes current evidence for provider/source health, public Hyperliquid TESTNET connectivity/rules, symbol registry, append-only ledger, durable watchdog and manual-route safety, plus explicit `DEFERRED` rows for private auth/account/order lifecycle, service liveness, host clock sync and the future shared `MockExchangeAdapter` replay boundary. See `STEP_44_NOTES.md` and `scripts/step44-global-readiness.sh`.


## Step 46 — Manual Control Safe Routing Contract

Step 46 completes the dashboard-side manual-control boundary without creating a trading command path. OPERATOR users can validate an `asset,weight_pct` CSV, preview the complete target delta (including omitted current assets as target zero), and evaluate a confirmed route admission. The server recomputes the request SHA-256, rejects stale reference targets, rechecks the explicit symbol registry/public venue rules, and appends the OPERATOR intent to an isolated durable audit store.

`POST /api/manual-control/route` always remains fail-closed in this phase: `submitted=false`, `routeEnabled=false`. PortfolioRisk manual transformation, the trading-control sink, Hyperliquid private authentication and submit/cancel/fill lifecycle are still `DEFERRED`. After Step 46 passes, the dashboard phase is functionally complete and the next safe phase is the deferred Step 37 private Hyperliquid TESTNET authentication. Run `./scripts/step46-manual-control-safe-routing.sh`.
