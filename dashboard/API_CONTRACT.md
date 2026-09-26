# Dashboard API contract draft — Step 14

The frontend-facing contract remains independent from PostgreSQL tables, NATS subjects and C++ service internals.

All browser traffic is same-origin under `/api`.

## Public operational endpoints

| Method | Endpoint | Purpose |
| --- | --- | --- |
| GET | `/api/health` | API process liveness. Returns `200` even if the configured real provider is not ready. |
| GET | `/api/readiness` | Data-provider readiness. Returns `503` while the real provider mapping is incomplete/unavailable. |

Example health shape:

```json
{
  "status": "ok",
  "service": "dashboard-api",
  "version": "0.14.1",
  "data_provider": {
    "name": "embedded-mock-fixtures",
    "mode": "mock",
    "ready": true,
    "detail": "server-side fixtures; no trading-system connectivity",
    "resourceCount": 12
  },
  "auth": "session-cookie"
}
```

## Authentication endpoints

| Method | Endpoint | Purpose |
| --- | --- | --- |
| POST | `/api/auth/login` | Verify credentials and create session. |
| GET | `/api/auth/me` | Return current user, role, CSRF token and expiry. |
| GET | `/api/provider-status` | Authenticated provider identity/readiness metadata for the UI source badge. |
| GET | `/api/integration-catalog` | Audited resource → owner/source/gap map from Step 14. |
| POST | `/api/auth/logout` | Revoke current session; requires `X-CSRF-Token`. |

The session identifier is held only in the HttpOnly `cd_session` cookie.

## Authenticated read endpoints

| DashboardDataSource method | HTTP endpoint | Response TypeScript model | Provider resource |
| --- | --- | --- | --- |
| `getShellStatus()` | `/api/shell-status` | `ShellStatus` | `shell-status` |
| `getOverview()` | `/api/overview` | `OverviewData` | `overview` |
| `getPositions()` | `/api/positions` | `PositionsData` | `positions` |
| `getReconciliation()` | `/api/reconciliation` | `ReconciliationData` | `reconciliation` |
| `getPipeline()` | `/api/pipeline` | `PipelineData` | `pipeline` |
| `getExecution()` | `/api/execution` | `ExecutionData` | `execution` |
| `getRisk()` | `/api/risk` | `RiskData` | `risk` |
| `getMarketData()` | `/api/market-data` | `MarketDataData` | `market-data` |
| `getInfrastructure()` | `/api/infrastructure` | `InfrastructureData` | `infrastructure` |
| `getAlertsAudit()` | `/api/alerts-audit` | `AlertsAuditData` | `alerts-audit` |
| `getLiveVsExpected()` | `/api/live-vs-expected` | `LiveVsExpectedData` | `live-vs-expected` |
| `getManualControl()` | `/api/manual-control` | `ManualControlData` | `manual-control` |

Successful data responses include:

```text
X-Dashboard-Data-Provider: embedded-mock-fixtures
```

## Provider behavior

### Mock

`DASHBOARD_DATA_PROVIDER=mock`

All 12 resources are served by embedded server fixtures through `provider.Provider`.

### Real read-only provider

`DASHBOARD_DATA_PROVIDER=real`

Real integrations are enabled resource-by-resource and remain read-only. Step 24 serves Overview, Infrastructure, Positions, Execution, Reconciliation, Pipeline/Why, Risk, Market Data and a derived Alerts/Audit evidence projection from canonical sources. Unwired resources continue to fail closed with HTTP `503`; there is never an automatic fallback to mock fixtures. `/api/health` is process liveness and must not be interpreted as trading readiness.

## Response rules

- Authenticated read success: HTTP `200`, JSON.
- Missing/expired session: HTTP `401`.
- Valid session without sufficient authorization on future control endpoints: HTTP `403`.
- Data provider unavailable/unverified: HTTP `503`.
- Login throttled: HTTP `429`.
- Operational responses use `Cache-Control: no-store`.
- Read endpoints do not accept POST.
- Browser-to-exchange, browser-to-NATS and browser-to-PostgreSQL access is forbidden.
- Future state-changing routes must require both server-side role authorization and CSRF validation.


## Step 14 integration catalog

`GET /api/integration-catalog` is authenticated and returns diagnostic mapping metadata only. It does not expose secrets, SQL access, NATS payload history or control capabilities. The endpoint exists to make integration status explicit while `RealProvider` remains fail-closed.

## Step 15 source diagnostics

Authenticated endpoint:

`GET /api/source-status`

In real-provider mode it reports bounded, read-only source reachability for PostgreSQL, NATS and the canonical SQLite market-data file. It is diagnostic only and does not imply that dashboard resources have been switched to live data.

## Step 16 real diagnostics

Authenticated:

```text
GET /api/runtime-state-summary
```

In real mode this returns a compact read-only PostgreSQL operational summary:

- database connection/size/table presence;
- runtime snapshot schema/update timestamp when present;
- cash/position/order/fill counts only;
- no raw snapshot payload and no mutation capability.

`GET /api/infrastructure` is the first normal dashboard resource enabled in `DASHBOARD_DATA_PROVIDER=real`. Fields whose canonical operational source is not wired yet are explicitly returned as `UNKNOWN`/`Not wired`; they are not filled with mock values.


## Step 17 real Positions

`GET /api/positions` is enabled in `DASHBOARD_DATA_PROVIDER=real`. Its canonical source is `trading_runtime_state.snapshot`.

Returned real fields include:

- durable account cash;
- non-zero physical `account_positions`;
- side/quantity derived directly from signed physical quantity;
- per-strategy `virtual_positions` breakdown;
- PostgreSQL persistence timestamp.

The response explicitly marks valuation, approved target, PnL and exchange reconciliation as unavailable. Numeric placeholders are never presented as real values by the Step 17 UI. Other not-yet-wired resources continue to return HTTP `503` in real mode.


## Step 18 update
`Execution` now has a bounded real read model from `trading_runtime_state.snapshot.orders` + `trading_fills`. Latency/slippage/replacement lineage remain unavailable unless a canonical durable source is added.


## Step 19 real Reconciliation contract

`GET /api/reconciliation` is enabled when `DASHBOARD_DATA_PROVIDER=real`. It combines only verified read-only sources:

- local durable state: `trading_runtime_state.snapshot`;
- exchange evidence: latest retained `execution.exchange.snapshot.v1` from JetStream stream `ALGOTRADING_RUNTIME`;
- runtime comparison semantics: verified C++ `Reconciler` defaults (`cash=1e-6`, `quantity=1e-10`).

The response includes `comparisonAvailable`, `evidenceFresh`, JetStream evidence sequence/time, per-asset alignment rows, durable local open orders, and concrete reconciliation issues. Missing/stale exchange evidence is `PENDING`; the dashboard does not request a new venue snapshot or write any trading state.


## Step 20 real Pipeline / Why contract

`GET /api/pipeline` is enabled in real mode. The read model is cycle-aligned: the latest PortfolioRisk decision checkpoint is the anchor; Strategy update and OrderPlanner request/plan are joined only when their `timestamp` equals that decision timestamp. ExecutionState and retained reconciliation evidence enrich the same cycle. The dashboard does not recompute RSI/rank or intermediate risk transforms and labels those fields unavailable when they are not durably persisted. Step 31 adds an optional `proof` block that correlates the same cycle through planned order IDs, durable runtime orders, `trading_fills`, and reconciliation evidence. `proof.status=ALIGNED` is emitted only for a submit-order cycle whose persisted chain is complete and internally consistent; quiet or in-flight cycles remain `PENDING` and contradictions/terminal failures are `BLOCKED`.


## Step 21 real Risk contract

`GET /api/risk` is enabled in real mode. It reads the latest cycle-aligned PortfolioRisk decision checkpoint and returns the persisted StrategyIntentBatch, AccountSnapshot and approved DecisionBatch boundary. It does not infer active policy limits, raw pre-risk sizing, binding constraints, volatility diagnostics, breach state, kill-switch state or global trading readiness.


## Step 22 real Market Data contract

`GET /api/market-data` in real mode is read-only and sourced from canonical SQLite plus the latest durable Strategy checkpoint. It returns the latest persisted ranking frontier, target-day freshness, bounded integrity diagnostics, a top-20 strategy universe selected by recomputed SMA Volume(25), recomputed RSI(7), and durable PureRSI signal state only when `strategy_market_update_checkpoint.timestamp` exactly matches the SQLite frontier. The dashboard does not join signals across different cycles.

Indicator recomputation mirrors the audited C++ implementation. Candidate-rejection wording remains a dashboard diagnostic projection because the runtime does not persist a first-class rejection-reason contract.


## Step 23 real Overview contract

`GET /api/overview` aggregates only already-verified read-only sources. Durable account state is required; optional component failures are surfaced in `sourceWarnings` rather than silently replaced with fixtures. `equityAvailable=false` and `liveExpectedAvailable=false` remain explicit until canonical ledger/valuation and historical baseline read models exist. `readiness` is never promoted to READY in Step 23; current reconciliation BLOCKED maps to PAUSED, otherwise the partial aggregate remains DEGRADED.


## Step 24 real Alerts & Audit contract

`GET /api/alerts-audit` is read-only and returns current operational conditions derived from already verified sources. `sourceMode=REAL`, `auditMode=DERIVED_SYSTEM_EVIDENCE`, `acknowledgementAvailable=false`, `durableAlertHistoryAvailable=false` and `humanAuditAvailable=false` are deliberate contract fields. The endpoint must not claim that current derived conditions are durable alerts or that reconstructed system evidence is a complete actor audit ledger.

## Step 25 — `/api/live-vs-expected` real-provider semantics

In `DASHBOARD_DATA_PROVIDER=real`, the endpoint is backed by a bounded read-only reconstruction from canonical SQLite market history. The latest completed day is the live observation and earlier observations form the rolling baseline. Unsupported replay/ledger/execution metric families are omitted rather than mocked.

## Step 26 — Manual Control preview

### `GET /api/manual-control`
Authenticated read endpoint. In real mode it returns the current manual-control capability envelope and a demo portfolio derived from the latest durable PortfolioRisk target. No mutation occurs.

### `POST /api/manual-control/preview`
Requires authenticated `OPERATOR` plus `X-CSRF-Token`.

Request:
```json
{
  "filename": "targets.csv",
  "csv": "asset,weight_pct\nBTCUSDT,50\nCASH,50"
}
```

Response is a non-mutating validation/target-delta preview with `validationPassed`, validation issues, deterministic `requestHash`, current approved target vs requested target rows, and `routeEnabled:false`.

There is intentionally no submit/route endpoint in Step 26.

## Step 27 — Real global shell/readiness

`GET /api/shell-status` is enabled in real-provider mode. In addition to the legacy shell fields it returns `exchangeState`, `exchangeDetail`, `dataState`, `dataDetail`, `tradingState`, `tradingDetail`, `readinessDetail`, `criticalAlertCount`, `warningAlertCount`, `blockers`, `warnings` and `sourceMode`.

Safety semantics are fail-closed: fresh retained exchange evidence does not imply live gateway connectivity; `tradingEnabled` remains false while no canonical global trading-enabled/paused contract exists; `READY` is not asserted until exchange connectivity, common service liveness/control state and host clock synchronization are independently observable.

## Step 28 streaming contract

`GET /api/stream` is authenticated with the same HttpOnly session cookie as the REST API and returns `text/event-stream`.

Events:

- `connected`: stream metadata (`version`, provider, transport and fallback cadence).
- `invalidate`: `{resources, subjects?, reason, at}`. Resource names match REST read models; `*` means refetch mounted resources.
- `auth-expired`: the current stream session is no longer valid.
- SSE comments are used as 15-second heartbeats.

NATS is an internal invalidation source only. The browser never receives the NATS connection or raw event payload as a data contract.

## Step 29 diagnostics / resilience contract

### `GET /api/diagnostics`

Authenticated, read-only dashboard-process diagnostics. It reports only dashboard API runtime/request/SSE metrics and provider identity. It is not a trading-service health authority and must not be used by the trading runtime as a dependency.

Operational limits:

- `DASHBOARD_RESOURCE_TIMEOUT` defaults to `7s` and bounds provider reads.
- `DASHBOARD_SSE_MAX_CLIENTS` defaults to `16` and bounds concurrent SSE clients.
- Excess SSE connections return `503 Service Unavailable` with `Retry-After: 3`.

## Step 30 production boundary

No new trading API contract is introduced. Production continues to expose the existing authenticated REST/SSE API only through Caddy. `dashboard-api` remains without a host-published port. The production smoke test verifies `/api/health`, authenticated `/api/source-status`, `/api/diagnostics`, and the `/api/stream` SSE handshake over HTTPS.


## Step 32 safety gate

`GET /api/safety-gate` is authenticated and read-only. It combines the real-provider shell, Step 31 proof, Manual Control `routeEnabled` state and dashboard auth posture into a fail-closed pre-testnet safety summary.

Important: `safeToProceed=true` means only that no verified contradiction blocks continued testnet integration work. It is **not** trading authorization. `tradingReady` remains a separate strict field and stays false until the full runtime readiness contract is independently satisfied.

## Step 33 venue foundation

`GET /api/venue-foundation` is authenticated and read-only. It exposes the configured venue integration identity without performing a venue network call.

Current Step 33 contract:

- `venue = HYPERLIQUID`
- `targetEnvironment = TESTNET`
- `gatewayMode = hyperliquid-dry-run`
- `publicConnectivity = NOT_CHECKED`
- `privateAuth = DISABLED`
- `orderRouting = DISABLED`
- `symbolMapping = NOT_CONFIGURED`
- `exchangeFilters = NOT_LOADED`
- `secretsRequired = false`
- `capitalRequired = false`
- `readOnly = true`

The endpoint is informational/configuration evidence only. It cannot publish NATS trading commands, sign venue requests, read private account state or create/cancel an order.

## Step 34 public venue status

`GET /api/venue-public` is authenticated and read-only. In real mode it performs two bounded public Hyperliquid TESTNET `POST /info` queries: `meta` and `allMids`.

The endpoint returns only a bounded summary:

- `status = CONNECTED_PUBLIC_ONLY | BLOCKED | MOCK_ONLY`
- `connected`
- `venue = HYPERLIQUID`
- `targetEnvironment = TESTNET`
- pinned public `/info` endpoint
- metadata/mids availability
- perpetual universe count
- public mids count
- metadata/mid overlap count
- a short sample of observed venue symbols
- bounded probe latency and check timestamp
- explicit safety fields (`privateAuth=DISABLED`, `orderRouting=DISABLED`, `secretsUsed=false`, `capitalUsed=false`, `readOnly=true`)

Step 34 does not query a user/account endpoint, does not sign a request, does not call the exchange action endpoint, and does not promote `Infrastructure.exchange.connected` or global trading readiness. Symbol mapping and venue rule application remain separate later gates.

## Step 35 — explicit venue symbol mapping

`GET /api/venue-symbol-map` is authenticated and read-only. It classifies every current canonical strategy symbol against an embedded, versioned `EXPLICIT_ONLY` manifest and current Hyperliquid TESTNET public `meta`. Symbols may be `VALID`, `UNSUPPORTED_EXPLICIT`, `UNSUPPORTED_VENUE_ABSENT`, or `MISSING_MAPPING`. Only `MISSING_MAPPING` is an unclassified hard failure; unsupported symbols are explicitly non-routable and keep execution coverage fail-closed. The response separates classification completeness from execution coverage with `classifiedRequiredCount`, `unsupportedRequiredCount`, `unsupportedInternalSymbols`, and `executionCoverageComplete`. No runtime suffix stripping, case conversion or alias guessing is allowed. Private auth, signing, account state, venue trading rules and order routing remain disabled.

## Step 36 — public venue trading rules

### `GET /api/venue-rules`

Authenticated, read-only. Available on the real provider and represented as `MOCK_ONLY` on the embedded mock provider.

Purpose: validate public Hyperliquid TESTNET precision/leverage/minimum-notional diagnostics for every Step 35-supported symbol without enabling order routing.

Important response fields:

- `status`: `VALIDATED` or `BLOCKED` (`MOCK_ONLY` in mock mode)
- `validated`
- `venue`, `targetEnvironment`
- `supportedMappingCount`, `nonRoutableMappingCount`
- `validatedRuleCount`, `blockedRuleCount`
- `priceMaxSignificantFigures=5`
- `perpMaxDecimals=6`
- `integerPricesAlwaysAllowed=true`
- `minOrderNotionalUsd=10`
- `rows[]` with exact symbol identity, `sizeDecimals`, size step, derived price decimal limit, max leverage, margin flags, delisted state, public mid and diagnostic minimum-size estimate
- `privateAuth=DISABLED`
- `orderRouting=DISABLED`
- `readOnly=true`

`POST /api/venue-rules` is not registered.

A validated response means only that current public rules are structurally usable for the supported TESTNET mappings. It is not permission to trade and is not a future order-rounding implementation.

### `GET /api/symbol-registry`

Authenticated, read-only Step 37A endpoint. It validates the versioned multi-exchange registry against the accepted Step 35 Hyperliquid TESTNET manifest, the current canonical Binance/source ranking window, the current strategy universe and current public Hyperliquid TESTNET metadata.

Important response fields include `registryVersion`, `registryArtifactSha256`, `currentRankingRegisteredCount`, `currentStrategyRegisteredCount`, `currentStrategyRoutableCount`, `unregisteredRankingSymbols`, `unregisteredStrategySymbols`, `venueAbsentStrategySymbols`, `mappingDivergences`, `alarms` and `rows`.

`validated=true` means every currently observed market-data/strategy symbol is explicitly registered and the registry agrees with the accepted Step 35 manifest. It does **not** mean every symbol is executable. `executionCoverageComplete=false` is expected when some entries are explicitly blocked or absent from the current venue metadata.

The endpoint never performs private authentication, signing or order submission. `POST /api/symbol-registry` is not registered.

## Step 42 — append-only ledger foundation

`GET /api/ledger` is authenticated and read-only. In REAL mode it reads the runtime-owned append-only `trading_fills` source through a fixed, bounded server-side query. Full-table aggregates validate `fill_id` uniqueness and basic fill economics; only a bounded recent window is returned to the browser.

Each fill is projected deterministically into one economic event (`fill:<fill_id>`) containing asset quantity delta, gross notional, commission and cash delta. A SHA-256 chain fingerprints the returned recent window. The fingerprint is diagnostic, not a substitute for a runtime-owned tamper-evident accounting store.

Step 42 intentionally returns `durableRealizedPnl=false`, `durableUnrealizedPnl=false` and `historicalEquityAvailable=false`. It does not infer cost basis or PnL from incomplete assumptions and exposes no write, wallet, signing or order route.

## Step 43 additions

`GET /api/alerts-audit` remains authenticated and GET-only. In real mode it may additionally expose watchdog/lifecycle fields: `watchdogAvailable`, `watchdogState`, `watchdogLastSweepAt`, `watchdogLastSuccessAt`, `durableEventCount`, and `durableLifecycleEvents`. The durable lifecycle is persisted by a separate no-port watchdog process to its own observability volume; it is not a trading-state store. Acknowledgement and durable human-action audit remain unavailable and no mutation route is introduced.

## Step 44 additions

### `GET /api/global-readiness`

Authenticated and read-only. Returns the versioned `step44-v1` global readiness contract.

Key fields:
- `contractComplete`
- `safeToContinueDashboard`
- `privateTestnetReady`
- `tradingReady`
- `liveReady`
- `orderRouting`
- `privateAuth`
- `manualRouting`
- `requirements[]` with `id`, `state`, `requiredNow`, `requiredFor`, `evidence`, `detail`, and `retryable`
- `futureReplayBoundary`

`VALIDATED_FAIL_CLOSED` explicitly means the current dashboard-development phase can continue with no verified hard contradiction. It does **not** authorize private testnet trading or LIVE capital.


## Step 45 — Live vs Expected real projection

`GET /api/live-vs-expected` remains authenticated and read-only. In REAL mode it returns `contractVersion=step45-v1`, a SHA-256 `baselineFingerprint`, explicit observation/baseline identity, anomaly classifications when enough history exists, and a coverage table. Contract validation and projection readiness are independent: `status=VALIDATED_LIMITED` + `validated=true` can coexist with `projectionReady=false` and `overallClassification=INSUFFICIENT_DATA` while fewer than five independent baseline observations exist. In that state `market-inputs=INSUFFICIENT_DATA` and no metric/anomaly classification is fabricated. Once sufficient history exists, `market-inputs=VALIDATED` and the five supported metrics are classified. Execution-distribution, performance/accounting and accepted-replay families remain `DEFERRED`. The projection never enables/disables routing and does not claim expected PnL.


## Step 46 — Manual Control safe routing admission

### `POST /api/manual-control/route`

Authenticated `OPERATOR` + CSRF endpoint. It is an **admission/audit endpoint only**, not a trading endpoint. Request fields are:

- `filename`
- `csv`
- `requestHash` from the immediately reviewed server preview
- `referenceTargetTimestamp` from the reviewed server preview
- `confirmation = CONFIRM_MANUAL_ROUTE`

The server recomputes the preview from the exact CSV, revalidates the current target timestamp, request SHA-256, explicit symbol-registry routability and current public venue-rule rows, and then persists a durable operator-intent event. In Step 46 the response is deliberately `status=BLOCKED`, `submitted=false`, `routeEnabled=false`; it never publishes to NATS, calls Hyperliquid, signs, submits/cancels an order, or writes trading PostgreSQL state.

Expected fail-closed blockers include `MANUAL_RISK_CONTRACT_UNAVAILABLE`, `TRADING_CONTROL_SINK_UNCONFIGURED`, `PRIVATE_AUTH_DEFERRED`, `ORDER_LIFECYCLE_DEFERRED`, and `GLOBAL_TRADING_READINESS_FALSE`. Hash/reference mismatches add `REQUEST_HASH_MISMATCH` / `STALE_REFERENCE_TARGET`.

### Manual operator-intent audit

`dashboard-api` owns the isolated append-only `/data/manual-audit/events.jsonl` store (`step46-v1`). It records actor, action, timestamp, request hash, correlation ID, reference target, blockers, result and `submitted=false`. `GET /api/manual-control` exposes a bounded recent view; `GET /api/alerts-audit` projects these records as HUMAN audit evidence. Alert acknowledgement remains separately deferred.
