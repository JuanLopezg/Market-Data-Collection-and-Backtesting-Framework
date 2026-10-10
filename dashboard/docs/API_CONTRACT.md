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
no explicit entry exists. Missing target entries mean HOLD, not zero/FLAT. Replay
rows describe retained observations and omit service end-to-end proof.

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
| `/api/overview` | Runtime account/fill aggregates and timestamped canonical valuation. |
| `/api/positions` | Durable quantities/orders/targets and explicit valuation provenance. |
| `/api/reconciliation` | Local runtime state compared with retained exchange evidence. |
| `/api/pipeline` | Cycle-aligned Strategy -> Risk -> Planner -> Execution lineage/proof. |
| `/api/execution` | Current durable orders and bounded fills. |
| `/api/risk` | Approved decision and exact persisted signals/account inputs. |
| `/api/market-data` | Canonical ranking/OHLCV, indicator reconstruction and aligned strategy signals. |
| `/api/infrastructure` | Bounded PostgreSQL/NATS/file diagnostics; optional fresh host telemetry adds CPU/RAM/disk, container health and `telemetryObservedAt`. `processRunning` is liveness, not `ready`; stale/missing measurements stay unknown. |
| `/api/alerts-audit` | Current derived alerts, watchdog lifecycle, audit and acknowledgement evidence. |
| `/api/live-vs-expected` | Versioned market/strategy-input anomaly projection with deferred coverage explicit. |
| `/api/manual-control` | Current capabilities/reference target and preview information. |

Additional authenticated GET diagnostics:

| Endpoint | Meaning |
| --- | --- |
| `/api/diagnostics` | API resource/timeout/read diagnostics. |
| `/api/source-status` | Bounded real source reachability; independent of process liveness. |
| `/api/runtime-state-summary` | Read-only runtime snapshot summary. |
| `/api/integration-catalog` | Source ownership/planning metadata; some labels retain historical integration stages. |
| `/api/safety-gate` | Explicit blockers before trading-control readiness. |
| `/api/global-readiness` | Aggregate readiness evidence; missing evidence cannot become ready. |
| `/api/venue-foundation` | Venue identity/config boundary and deferred capabilities. |
| `/api/venue-public` | Public-only Hyperliquid TESTNET metadata/connectivity status. |
| `/api/venue-symbol-map` | Explicit canonical-to-venue symbol mapping. |
| `/api/venue-rules` | Public precision/minimum/rule validation. |
| `/api/symbol-registry` | Multi-venue registry and drift diagnostics. |
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
evaluates admission and appends an isolated durable operator-intent audit.
It always reports `submitted=false` and `routeEnabled=false`.
An admission/audit success is not an order submission. Private authentication, manual
risk transformation, trading-control delivery and real order lifecycle remain deferred.

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
