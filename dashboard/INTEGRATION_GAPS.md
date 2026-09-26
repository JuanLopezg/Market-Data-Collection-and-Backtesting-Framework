# Integration gaps discovered in Step 14

This file is intentionally explicit about what the existing trading code does **not** yet expose to the dashboard.

## 1. PnL / ledger

Available now:
- current cash/positions
- append-only fills with commission
- canonical market prices

Missing:
- durable cost basis / realized PnL ledger
- attributed unrealized PnL read model
- historical equity series optimized for dashboard queries

Do not fabricate PnL from incomplete assumptions in React.

## 2. Reconciliation status

Available now:
- normalized ExchangeSnapshot
- Reconciler and ReconciliationReport
- clean/blocked behavior inside ExecutionState

Missing:
- durable latest reconciliation report
- timestamped status readable independently by dashboard-api

Recommended later change: publish/persist a normalized reconciliation-status projection owned by ExecutionState.

## 3. Risk explainability

Available now:
- approved DecisionBatch
- portfolio config limits
- generic sizing diagnostic types elsewhere in the codebase

Missing in distributed live boundary:
- raw pre-risk target
- volatility scaling factor for each live cycle
- exact asset-cap/gross-cap binding evidence
- before/after weights per constraint

Recommended later change: extend the PortfolioRisk durable decision checkpoint with a diagnostics payload or introduce a versioned risk-decision diagnostic contract. Do not re-run PortfolioRisk in the dashboard and claim it is authoritative.

## 4. Infrastructure readiness

Available now:
- PostgreSQL health can be checked
- NATS monitor/JetStream can be checked
- container topology is known

Missing:
- normalized service heartbeat/readiness contract
- safe host/container metric source

Avoid giving dashboard-api broad Docker socket access just to display container metrics.

## 5. Alerts/Audit

Available now:
- structured logs
- correlation/message IDs

Missing:
- durable alert records
- acknowledgement lifecycle
- durable human-action audit records

A dedicated projection/append-only table is preferable to parsing logs on every page load.

## 6. Live vs Expected

Step 45 closes the dashboard-side real projection contract for the currently supportable slice. The read model is versioned as `step45-v1`, fingerprints the exact rolling baseline input/configuration, excludes the latest completed canonical day from baseline statistics, and exposes deterministic z-score classifications plus explicit anomaly rows.

Available now:
- canonical `market_volume_rank_daily` + `ohlcv_data` rolling market/strategy-input baseline;
- defined baseline window/config identity via SHA-256 fingerprint;
- latest completed canonical observation vs historical distribution;
- explicit NORMAL / ELEVATED / ABNORMAL / CRITICAL classifications;
- explicit coverage contract showing unsupported families as DEFERRED;
- read-only UI projection with no routing side effects.

Still missing by design:
- canonical versioned execution-distribution baseline (slippage/latency/rejects);
- runtime-owned realized/unrealized PnL and historical-equity accounting baseline;
- holding-time baseline;
- accepted versioned full-replay baseline artifact.

These gaps must remain visible rather than being reconstructed or inferred by the dashboard.

## 7. Manual Control

Step 46 now adds the authenticated OPERATOR + CSRF confirmed routing-admission contract on top of the Step 26 preview. The server binds the exact CSV to a SHA-256 and current target reference, rechecks explicit registry/public venue-rule evidence, and persists operator intent in an isolated append-only audit store. It remains deliberately non-executing (`submitted=false`, `routeEnabled=false`). Still missing by design are the authoritative trading-control sink/manual PortfolioRisk transformation, private Hyperliquid authentication, and submit/cancel/fill/reconciliation lifecycle. Durable manual-intent audit is no longer a gap; generic alert acknowledgement remains deferred.

## Step 23 update

Overview can now aggregate the verified real read models, but canonical marked equity, PnL/attribution, historical equity series, Live-vs-Expected baseline and full global readiness remain deliberate gaps. The Overview exposes these as unavailable/degraded rather than using mock or inferred values.


## Step 24 update

Alerts & Audit is now **PARTIAL** rather than missing: current alerts are derived read-only from real source health, reconciliation, execution, market-data and risk evidence. Remaining product gaps are durable append-only alert history, acknowledgement/resolution state, and a durable human-action audit model.

## Step 27 update — Global readiness

The dashboard can now aggregate verified source health, durable runtime-state presence, canonical market-data diagnostics, retained reconciliation evidence and derived active-alert counts into the global shell. The remaining blockers to a true `READY` verdict are independent exchange-gateway connectivity, a normalized common service heartbeat/trading-control state, and host clock-sync evidence. Step 27 therefore remains intentionally fail-closed at `DEGRADED` when all currently observable sources are healthy, and `PAUSED` when a verified hard blocker exists.

## Step 28 update — Streaming invalidations

The browser now receives authenticated SSE invalidations from `dashboard-api`. The backend uses a read-only raw NATS subscription to the audited runtime subjects, maps each subject to affected dashboard REST read models, and coalesces bursts before notifying the browser. The browser still reads all actual page data through the existing REST projections; it never receives a NATS connection or treats raw event payloads as a UI contract. A 30-second fail-safe refresh covers PostgreSQL/SQLite-only changes and dropped burst invalidations. This closes the aggressive-polling gap, but it does not create a durable event/audit store or any command/control NATS path.

## Step 34 update

Closed: public Hyperliquid TESTNET reachability and basic perpetual metadata/mids visibility are now observable through a bounded read-only adapter.

Still open after Step 34:

- explicit internal asset -> Hyperliquid execution symbol mapping;
- venue precision/minimum-order/trading-rule application;
- private authentication/account snapshot;
- actual ExchangeGateway public/private connectivity contract owned by the trading runtime;
- submit/cancel and fill ingestion;
- testnet reconciliation and ledger.

## Step 42 update — ledger foundation, not final accounting ledger

Step 42 closes the deterministic read-only fill-ledger foundation: append-only `trading_fills` is exposed as immutable fill-derived economic events with explicit asset/cash deltas and integrity diagnostics. The remaining accounting gap is narrower but still material: a runtime-owned durable cost-basis / realized-PnL projection, attributed unrealized PnL, and historical equity series are still required before the dashboard can claim canonical PnL/accounting parity.

## Post-Step-44 explicit deferred readiness contracts

Step 44 makes these gaps explicit instead of inferring readiness around them:
- Hyperliquid private API wallet authentication (Step 37 deferred)
- private account/balance/open-order snapshot (Step 38 deferred)
- submit/cancel and private fill lifecycle (Steps 39–40 deferred)
- durable reconciliation against private TESTNET evidence (Step 41 deferred)
- common trading-service heartbeat/control state
- host/VPS clock synchronization telemetry
- future `MockExchangeAdapter` implementing the same canonical venue contract as Hyperliquid for full replay + dashboard
