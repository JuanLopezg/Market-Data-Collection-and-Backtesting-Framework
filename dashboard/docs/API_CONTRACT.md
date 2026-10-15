# Dashboard API contract

Checked against `dashboard-api/internal/server/server.go` and current provider
handlers on 2026-10-08. Browser requests are same-origin under `/api`.
The frontend contract is independent of runtime SQL schemas and NATS subjects.

Market Data adds optional `liquidityLabel` without changing `smaVolumeLabel`.
PAPER returns `SMA Quote Volume 25 (USDT)` and actual 25-row mean quote turnover.
Required missing quote rows contribute to integrity issues. A pre-cutover durable
signal checkpoint is not joined into the new metric preview (`signalCycleAligned`
is false). `DASHBOARD_QUOTE_VOLUME` selects the metric and
`DASHBOARD_QUOTE_VOLUME_FROM` identifies the first completed cycle under it. The
rolling input baseline excludes pre-cutover cycles and requires six observations.

## Operational and authentication endpoints

Pipeline adds optional `evidenceState` (`OBSERVED`, `PARTIAL`, `PENDING`) and
`emptyReason`. A missing first decision returns a structured pending response;
database/decoding/timestamp failures remain errors. Signals can be `UNKNOWN` when
no explicit entry exists. Missing target entries mean HOLD, not zero/FLAT. Rows add
optional `decisionLabel` (ENTER, RESIZE, EXIT, HOLD), `targetLabel` (approved USD,
"—" on HOLD), `inUniverse`, `signalValue` (aggregated signal in [-1, +1], strategies
weighted by capital share) and `strategies` (per-strategy capital, share, signal,
indicator, sized/approved weight and decision); the response adds optional `summary` cards. Replay states
with a `pipeline` block (latest close decision: liquidity rank, RSI, signal, sized/capped/
approved weights, rebalance action, target, current position, planned orders) give the
full lineage (`evidenceState` OBSERVED); older replay states describe retained
observations only. Replay rows never carry service end-to-end proof. PAPER rows take
sized/approved weights and the action from the same-cycle risk evaluation; RSI and
liquidity rank are not persisted there and show "—".

Risk adds optional `evidenceKind` (`DURABLE_DECISION`, `REPLAY_SNAPSHOT`, `PENDING`),
`decisionDetail` and `policies`. Policy fields expose persisted strategy allocation,
sizer, gross/asset caps and rebalance settings, separately from evaluated breaches.
Replay uses the observational risk view; missing policy/decision fields stay
unavailable and do not imply zero risk or an armed kill switch.

New service cycles add optional `configurationFingerprint` (SHA-256 of the persisted
cycle configuration identity) and `riskEvaluations`. Each strategy includes sizing
availability, capital, asset/gross caps, gross scaling, annualized volatility
observations when available, and asset weights before/after each constraint. Asset
rows carry current quantity, rebalance action and ASSET_CAP/GROSS_CAP reduction
reasons. HOLD does not emit the evaluated weight as a new target. Missing sizing or
volatility stays unavailable; EqualWeight volatility is not applicable. Old schemas,
old rows and canonical replay snapshots omit these fields. Malformed or mismatched
cycle/configuration reports are errors. Whole-account/venue breaches remain separate.

| Method | Endpoint | Access and meaning |
| --- | --- | --- |
| GET | `/api/health` | Public process liveness; 200 does not mean trading/source readiness. |
| GET | `/api/readiness` | Public provider readiness; the real provider currently remains unready. |
| POST | `/api/auth/login` | Credentials create a session and CSRF token; throttled after failed attempts. |
| GET | `/api/auth/me` | Session user, role, CSRF token and expiry. |
| GET | `/api/provider-status` | Authenticated provider identity/readiness metadata. |
| POST | `/api/auth/logout` | Authenticated session revocation; CSRF required. |

The opaque session is held in the HttpOnly `cd_session` cookie. Roles and CSRF
are checked by the server. See [AUTH_SECURITY.md](AUTH_SECURITY.md).

## Authenticated resource endpoints

All following endpoints use GET and require a valid session.

| Endpoint | Read model |
| --- | --- |
| `/api/shell-status` | Global observed source/data/trading status with blockers and warnings. |
| `/api/overview` | Runtime account/fill aggregates and timestamped canonical valuation. Stats: Total Equity, Cash, (Unrealized PnL), Leverage (gross position value / equity); Kraken LIVE adds BTC Collateral (after haircut), Sizing Capital (equity + collateral), Margin Headroom and Drop to Maintenance (from `DASHBOARD_KRAKEN_ACCOUNT_FILE`, the account monitor's `account.json`, stale after 5 min) and Net Deposits (`trading_equity_daily.capital_flows`). `equityCurve[]`: `{label, equity, balance?, closeLoop?}` (balance = equity minus open-position PnL; closeLoop = that day's reconciliation state: CLEAN or BLOCKED, BLOCKED if any check that UTC day found issues; real runtime from `trading_reconciliation_daily` since 2026-10-14, earlier days omitted, not assumed clean). `liveExpected[]`: `{label, equity, expected?}` (fast backtest; PAPER from the daily comparison, replay from `expected_equity.csv`). |
| `/api/positions` | Durable quantities/orders/targets and explicit valuation provenance. |
| `/api/reconciliation` | Our books compared with the exchange, per asset: `rows[]` `{asset, localQty, exchangeQty, deltaQty (exchange - ours), pendingQty (signed open-order remainder), status}`, `openOrders[]`, `issues[]`, overall `status`. PAPER/LIVE: durable runtime state vs the latest retained exchange snapshot (PENDING when that snapshot is older). Simulation: the reconciler's local ledger (sum of fill position deltas) vs the MOCK account; overall verdict and issues from the C++ reconciler. |
| `/api/pipeline` | Cycle-aligned Strategy -> Risk -> Planner -> Execution lineage/proof. |
| `/api/execution` | Current durable orders and bounded fills; optional `slippageHistory` (`{label, bps, fills}` per day, notional-weighted, positive = adverse) when the expected price is known. `rejects[]` splits the gateway's `CODE: text` message into `code`, `title`, `reason` and `remediation`; `kind` is `SKIPPED` for `MARKET_UNAVAILABLE`, `MARKET_NOT_TRADING` and `BELOW_MINIMUM_QUANTITY` (order state `SKIPPED`, counted in `skippedCount`, not in `rejectCount`) and `REJECTED` otherwise. Alerts: a skip is INFO `ORDER_SKIPPED`, `NOTIONAL_MISMATCH` is CRITICAL, other rejects WARN. |
| `/api/risk` | Approved decision and exact persisted signals/account inputs; optional page view `summary` (cards with `tone`), `history` (`{label, leverage?, drawdown}`), `strategyLimits` and `exposures`. |
| `/api/market-data` | Canonical ranking/OHLCV, indicator reconstruction and aligned strategy signals. |
| `/api/infrastructure` | Bounded PostgreSQL/NATS/file diagnostics; optional fresh host telemetry adds CPU/RAM/disk, container health and `telemetryObservedAt`. `processRunning` is liveness, not `ready`; stale/missing measurements stay unknown. |
| `/api/alerts-audit` | Current derived alerts, watchdog lifecycle, audit and acknowledgement evidence. Simulation: notices for route safety, reconciliation, leverage (>1.5x warn, >2.5x critical), drawdown (<-10% warn, <-25% critical), rejected orders and manual-target state, each with the date the condition started; never stops trading. |
| `/api/live-vs-expected` | `paperComparison` (equity vs fast-backtest expected equity per day, and positions vs expected) is what the page shows; the anomaly projection fields remain for compatibility. Kraken LIVE rows add `liveFees`, `expectedFees`, `liveFunding`, `liveInterest`, `capitalFlows`, `slippage` (USD totals since the study started) and `skipped` (assets Kraken skipped that cycle); the page then shows where the difference comes from. |
| `/api/manual-control` | Current capabilities/reference target and preview information. |

Additional authenticated GET diagnostics:

| Endpoint | Meaning |
| --- | --- |
| `/api/diagnostics` | API resource/timeout/read diagnostics. |
| `/api/infrastructure/history?range=1h\|6h\|24h\|7d` or `?from=…&to=…` (RFC 3339, last seven days) | Host CPU (% of host capacity), RAM (MiB) and disk (GiB) over time with per-container CPU/RAM, read from `history.jsonl` of `deploy/chain/monitor.py` (`DASHBOARD_HOST_HISTORY_FILE`). Averaged into at most 720 buckets; absent values stay absent; `stale` when the newest row is older than two minutes; `available: false` with a reason when not configured. |
| `/api/source-status` | Bounded real source reachability; independent of process liveness. |
| `/api/runtime-state-summary` | Read-only runtime snapshot summary. |
| `/api/integration-catalog` | Source ownership/planning metadata; some labels retain historical integration stages. |
| `/api/safety-gate` | Explicit blockers before trading-control readiness. |
| `/api/global-readiness` | Aggregate readiness evidence; missing evidence cannot become ready. |
| `/api/venue-foundation` | Venue identity/config boundary and deferred capabilities. |
| `/api/venue-public` | Public-only Hyperliquid TESTNET metadata/connectivity status. |
| `/api/venue-symbol-map` | Explicit canonical-to-venue symbol mapping. |
| `/api/venue-rules` | Public precision/minimum/rule validation. |
| `/api/symbol-map` | Binance -> Kraken symbol table checked against current prices (Symbol Mapping page; alerts `SYMBOL_MAP_*`). |
| `/api/ledger` | Deterministic fill-derived economic projection, not durable cost-basis/PnL. |
| `/api/stream` | Authenticated SSE invalidations and heartbeat. |

SSE notifications trigger resource refreshes; they do not expose raw NATS messages.
Disconnect/reconnect does not mutate trading. Browser visibility/polling is technical
observability time, not the market's economic clock.

## Operator POST endpoints

`POST /api/manual-control/preview` validates bounded CSV/target input server-side
against the current reference. It requires OPERATOR and `X-CSRF-Token`. It does
not publish a trading command or alter trading state.

`POST /api/manual-control/route` checks the preview/request hash and reference,
evaluates admission and appends an isolated durable operator-intent audit. With the
real provider outside the trading chain it always reports `submitted=false` and
`routeEnabled=false`: an admission/audit success is not an order submission.

Real provider in the trading chain (`DASHBOARD_RUNTIME_MODE=PAPER`, or LIVE with
`DASHBOARD_EXECUTION_VENUE=KRAKEN`, and `DASHBOARD_MANUAL_REQUEST_DIR` set):

- Same CSV rules and response fields as the replay simulation below. Limits are the
  strictest strategy's `max_asset_weight`/`max_gross_leverage` of the risk checkpoint's
  portfolio config; tradable assets are the top-N universe plus held assets (PAPER) or the
  Kraken contracts verified on the Symbol Mapping page (LIVE). Current weights use the
  latest daily closes.
- `routeBlockers` can contain `RISK_LIMITS_UNAVAILABLE`, `EXECUTION_STATE_UNAVAILABLE`,
  `ORDERS_IN_FLIGHT`, `DAILY_CYCLE_WINDOW` (23:45-00:15 UTC), `REQUEST_IN_PROGRESS`,
  `EQUITY_UNAVAILABLE` and, in LIVE unless `DASHBOARD_LIVE_ORDER_SUBMISSION=enabled`,
  `LIVE_ORDER_SUBMISSION_DISABLED`.
- `route` writes a `MANUAL_REQUEST_V1` file (mode 0644; format in
  `live_trading/execution_state_service/src/manual_requests.h`) to `<dir>/requests` and
  answers `QUEUED` with `submitted=true`; execution-state executes or rejects it within
  seconds and writes the outcome to `<dir>/processed`. Each route (queued or blocked) is
  appended to the manual audit store (action `MANUAL_REBALANCE`).
- `requests[]` stages: QUEUED (file waiting), EXECUTED (checked, orders being sent),
  AT_EXCHANGE, FILLED, RESYNCED (the next daily cycle has executed) or FAILED (rejected by
  execution-state, or an order rejected/canceled by the venue, with its reason).

Replay simulation provider only (`DASHBOARD_DATA_PROVIDER=simulation`):

- `preview` accepts `asset,weight_pct` (also `;` + decimal comma and a UTF-8 BOM, as
  exported by Excel). `CASH` is optional (the remainder stays in cash, negative when
  levered), weights must respect the portfolio-risk limits published by the replay
  (`maxGrossPct`, `maxAssetPct`; rejected, never clipped) and every asset must be in
  `routableAssets`. `previewRows[].action` is BUY/SELL/CLOSE/HOLD; held assets missing
  from the file are shown as CLOSE.
- `route` re-validates the CSV, requires the previewed `requestHash` and writes a
  `REPLAY_MANUAL_V1` request (`action=SET`, `target=ASSET,FRACTION`) atomically into
  `<simulation dir>/requests`. Status `QUEUED` means queued, not executed: the replay
  executes the newest request at its next price (open for overnight requests, close for
  daytime ones); the end-of-day decision then returns to the strategy targets.
- `GET /api/manual-control` adds `acceptingRequests`, `maxGrossPct`, `maxAssetPct` and
  `requests[{id, stage, detail, orders, filled}]` (newest first; stage QUEUED, EXECUTED,
  AT_EXCHANGE, FILLED, RESYNCED or FAILED).
- The replay appends `result` (EXECUTED/REJECTED/SUPERSEDED), `detail`, `replay_date` and
  `orders=<first>-<last>` to each request and moves it to `<simulation dir>/processed`;
  `/api/alerts-audit` lists those as the audit log.

`GET /api/reports/closed-positions.csv[?year=YYYY]` (any authenticated role) downloads
closed positions: asset, side, opened_at, closed_at, quantity, entry_price, exit_price,
cost_basis, proceeds, gross_pnl, fees, funding, net_pnl (USD; year = UTC close year).
503 when the trade history does not fit one read. `/api/positions` adds
`closedPositions[]`, `closedAvailable`, `fundingAvailable` and per open position
`openedAt`, `feesUsd`, `fundingUsd`, `netPnlUsd`, `feesAvailable`, `fundingAvailable`.

`POST /api/alerts-audit/acknowledge` requires OPERATOR and CSRF, with JSON:

```json
{
  "alertId": "current-alert-id",
  "acknowledgementKey": "current-durable-lifecycle-event-id",
  "comment": "Operator observation"
}
```

The body is bounded, unknown fields are rejected, and comments are limited to 500 bytes.
The alert must still be active and bound to the exact current durable lifecycle event.
A stale key returns conflict. Repeating an already recorded acknowledgement for that
lifecycle returns the existing record. Persistence must succeed before acknowledgement
is reported. ACK leaves `resolutionState=UNRESOLVED` and
`tradingStateMutated=false`; it does not remove a readiness blocker.

## Response and provider rules

- Successful authenticated reads return JSON with provider identity in
  `X-Dashboard-Data-Provider`.
- Missing/expired session: 401. Insufficient role or invalid CSRF: 403.
- Invalid requests: 400; missing current alert: 404; stale lifecycle/reference: 409.
- Login throttling: 429. Unavailable source/store/integration: 503.
- `Cache-Control: no-store` applies to operational responses.
- An available read model can return 200 with PENDING/BLOCKED/unavailable fields;
  HTTP success is not evidence of complete accounting or trading readiness.
- Real mode never falls back to mock. Simulation reads canonical published replay state.
- Unknown price/PnL/liveness/risk values remain unavailable; zero is not a substitute.

Pipeline joins use the risk decision timestamp as the anchor. Strategy/planner data
must match it exactly. An ALIGNED proof requires a complete persisted submit-order
chain; quiet/in-flight cycles remain PENDING and contradictions block.
Retained exchange snapshots carry age/provenance and are not inferred to be current.

Public venue requests use fixed public TESTNET metadata/mid-price operations.
They use no private signing/order endpoint. No browser-to-exchange route exists.

## Implementation references

Infrastructure optionally includes `tradingProgress` for persisted decision/plan
dates, expected daily PAPER dates, persistence time, state and explanation. Daily
freshness uses completed close yesterday and plan application today, with a
30-minute UTC rollover grace. A no-order plan is valid progress; application is
not fill/reconciliation proof. Other modes remain UNKNOWN rather than comparing
historical business time to today's wall clock.
Host telemetry optionally includes external `processes` observations. Service
`processRunning` is omitted when unavailable; `processState` distinguishes RUNNING,
STOPPED, MISSING and UNKNOWN. Container running does not imply executable presence
or an application heartbeat. `clockObserved` gates host NTP SYNCED/UNSYNCED versus
UNKNOWN; `clockOffsetLabel` does not claim a measured offset. Stale telemetry
discards process and clock evidence with the host resource readings.

- [server.go](../dashboard-api/internal/server/server.go): registered routes, authorization and action responses.
- [provider/real.go](../dashboard-api/internal/provider/real.go): enabled real reads and provider readiness.
- [REAL_DATA_SOURCE_MAP.md](REAL_DATA_SOURCE_MAP.md): source ownership and unresolved accounting gaps.
- [dashboard README](../README.md): run/build commands and provider selection.

Resource structs and frontend types are the detailed field schema. Update both with an
API change; avoid keeping a second hand-copied JSON schema in this guide.
