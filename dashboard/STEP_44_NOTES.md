# Step 44 — Full Global Readiness Contract

Step 44 replaces implicit "green badge" reasoning with a single explicit, phase-scoped readiness contract exposed by `GET /api/global-readiness`.

The important semantic split is intentional:

- `safeToContinueDashboard=true` means the current dashboard-development phase has no verified hard contradiction.
- `privateTestnetReady=false` remains false while Step 37 and Steps 38–41 are deferred.
- `tradingReady=false` remains fail-closed.
- `liveReady=false` remains fail-closed.

A Step 44 PASS therefore **does not authorize trading**.

The contract enumerates current and deferred dependencies with stable IDs, evidence sources, state (`PASS`, `WARN`, `DEFERRED`, `BLOCKED`), scope, and whether a blocker is retryable. Only bounded, recognized read/transport failures are retryable; semantic contradictions are not.

Current-phase evidence includes the real provider, PostgreSQL/NATS read paths, canonical market data, durable Step 31 proof, Hyperliquid TESTNET public foundation/connectivity/rules, the multi-exchange symbol registry, Step 42 ledger, Step 43 durable watchdog/critical-alert state, and fail-closed manual routing.

Explicitly deferred dependencies include Hyperliquid private auth, account snapshot, private order lifecycle/reconciliation, common service liveness/control, host clock sync, and the future mock-exchange replay adapter.

The future replay invariant is recorded in the contract: `HyperliquidAdapter` and a future `MockExchangeAdapter` must implement the same canonical venue boundary so a full replay drives the same persistence/read models/dashboard rather than a dashboard-only shortcut.

No wallet, private key, signing, `/exchange`, submit/cancel endpoint or capital movement is added by Step 44.
