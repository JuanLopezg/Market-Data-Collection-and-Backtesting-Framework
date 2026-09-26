# Step 33 — Testnet Integration Foundation

Step 33 is the first post-safety-gate venue step. It **does not connect to an exchange** and it does not add any secret, signing code, private endpoint or order-routing capability.

## Why Hyperliquid is the current foundation

The supplied current C++ `live_trading/exchange_gateway/src/exchange_gateway_main.cpp` already exposes two modes:

- `backend` for the existing backend/replay path;
- `hyperliquid-dry-run` for a prepare-only boundary.

The dry-run path consumes the durable `NotionalOrderPlan` boundary, calculates raw quantity for inspection and explicitly reports `real_submission=false`. It does not bind the SubmitOrder/CancelOrder/backend path in that mode.

Step 33 therefore follows the **current source tree** rather than the older product mock examples that named Binance.

## What changed

- Version bumped to `0.33.0`.
- New real-provider configuration identity:
  - `DASHBOARD_EXECUTION_VENUE=HYPERLIQUID`
  - `DASHBOARD_VENUE_TARGET_ENVIRONMENT=TESTNET`
  - `DASHBOARD_EXCHANGE_GATEWAY_MODE=hyperliquid-dry-run`
- Configuration fails closed if those values are changed during this step.
- New authenticated, read-only endpoint: `GET /api/venue-foundation`.
- New **Step 33 · Testnet Integration Foundation** panel in Infrastructure.
- Infrastructure and the global shell now name the configured execution venue instead of using the generic `VENUE` placeholder.
- Exchange connectivity remains `UNKNOWN` / not connected until Step 34 performs a public connectivity check.
- New acceptance script: `./scripts/step33-testnet-foundation.sh`.

## Safety boundary

Step 33 intentionally reports:

- `publicConnectivity = NOT_CHECKED`
- `privateAuth = DISABLED`
- `orderRouting = DISABLED`
- `symbolMapping = NOT_CONFIGURED`
- `exchangeFilters = NOT_LOADED`
- `secretsRequired = false`
- `capitalRequired = false`
- `readOnly = true`

This means **foundation ready**, not exchange ready and not trading ready.

## Local WSL acceptance

Start the normal local stack as before, then run:

```bash
./scripts/step33-testnet-foundation.sh
```

The script requires the Step 32 safety endpoint to remain safe-to-proceed, audits the current C++ gateway dry-run markers, runs the Go regression gate, verifies no exchange-secret configuration or command route was added, checks `/api/venue-foundation`, confirms Infrastructure does not claim connectivity, and confirms Manual Control remains fail-closed.

## Next step

Step 34 may add **public venue connectivity / public metadata**. It must still use no API key, no signing, no private account endpoint and no order submit/cancel.

## v0.33.1 — bounded startup stabilization for Step 32 precondition

The Step 33 acceptance script now tolerates only one bounded startup race: immediately after recreating `dashboard-api`, canonical SQLite market-data diagnostics may briefly be unavailable while the live MarketData/WAL sidecars settle. The script retries that exact single blocker for up to 15 attempts at 2-second intervals.

Safety is not weakened: any other `BLOCKED` Step 32 condition fails immediately. If market-data diagnostics remain unavailable after the bounded retry, the script fails closed and prints `/api/source-status` and `/api/market-data` responses so the underlying SQLite/source error is visible.
