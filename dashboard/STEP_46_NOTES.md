# Step 46 — Manual Control Safe Routing Contract

Step 46 completes the **dashboard-side** manual-control boundary. It does not complete the trading-side manual route.

## What is now real

- `POST /api/manual-control/preview` remains authenticated OPERATOR + CSRF and server-authoritative.
- Preview now compares the union of requested and current target assets. Omitting a current asset therefore means an explicit requested target of zero; it cannot silently disappear from the delta.
- Non-CASH target deltas are checked against the current explicit multi-exchange symbol registry and public Hyperliquid TESTNET venue-rule projection.
- `POST /api/manual-control/route` adds explicit confirmation, exact request SHA-256 binding, stale-reference checking, and a fresh server-side preview/constraint recheck.
- Every route-admission attempt is persisted to the isolated append-only `step46-v1` operator-intent audit store.
- Alerts & Audit exposes those records as durable HUMAN audit evidence.
- Step 44 global readiness recognizes the Step 46 dashboard contract and reports phase `DASHBOARD_COMPLETE_PRIVATE_TESTNET_DEFERRED` when all other current evidence is healthy.

## What is deliberately still absent

The Step 46 route endpoint **never submits**. It always returns `submitted=false` and `routeEnabled=false` because these trading-side prerequisites remain deferred:

- authoritative manual PortfolioRisk transformation/approval;
- trading-control command sink owned by the trading runtime;
- Hyperliquid private TESTNET authentication;
- private account/open-order state;
- submit/cancel/fill lifecycle and private reconciliation.

The dashboard has no NATS trading publish, exchange action call, signer/private key, order creation/cancel path, or trading PostgreSQL write.

## Durable manual intent store

`dashboard-api` owns `/data/manual-audit/events.jsonl` in the `dashboard-manual-audit-data` named volume. The watchdog does not mount/write this volume. Events include actor, timestamp, exact request hash, correlation ID, reference target timestamp, blockers, result and `submitted=false`.

This is audit/observability state only; it is not an economic/trading source of truth.

## Next phase

After the Step 46 gate passes, the dashboard phase is functionally complete. Resume the deferred Step 37 Hyperliquid private TESTNET authentication. Real capital is still not required at that point.
