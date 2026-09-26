# Step 42 — Append-Only Ledger Foundation

Status target: **foundation only**. This step deliberately skips deferred private venue Steps 37-41 and does not require a wallet or funds.

## What this step adds

- `GET /api/ledger` (authenticated, GET-only).
- A bounded, read-only PostgreSQL query over runtime-owned `trading_fills`.
- Full-table integrity/economic aggregates: row count, distinct `fill_id`, invalid-row count, fees, gross buy/sell notional, latest fill identity/timestamp.
- Deterministic fill-derived economic entries with:
  - immutable `entryId = fill:<fill_id>`;
  - explicit asset quantity delta;
  - explicit cash delta including commission;
  - deterministic SHA-256 chain over the returned recent window.
- An Execution-page panel showing the current ledger foundation and its bounded recent entries.
- `scripts/step42-ledger-foundation.sh`.

## Safety boundary

The dashboard remains read-only. It does **not** create or mutate an accounting table, submit/cancel orders, use wallet credentials, or call a private exchange endpoint.

The verified runtime contract already defines `trading_fills` as append-only by `fill_id`; Step 42 projects that durable source. A duplicate fill identity, malformed economic row, non-finite economics, unavailable source, or writable/command surface blocks the Step 42 gate.

## What Step 42 does NOT claim

This is **not yet a complete accounting/PnL ledger**. The current runtime does not persist first-class durable cost basis, realized PnL attribution, attributed unrealized PnL, or a historical equity series. The API therefore keeps:

- `durableRealizedPnl=false`
- `durableUnrealizedPnl=false`
- `historicalEquityAvailable=false`

Those fields must not be promoted until a runtime-owned accounting projection exists and is independently reconciled.

## Deferred venue steps

Step 37 private TESTNET auth and Steps 38-41 remain deferred until a suitable funded/onboarded TESTNET account is available. Step 42 requires no wallet and no capital.

## Next

Step 43 can add durable Alerts / Watchdog foundations, including symbol-registry drift alarms and ledger-integrity conditions, without enabling order routing.

## Fix 0.42.2
The Step 37A regression gate now distinguishes a transient `WARN SYMBOL_REGISTRY_BLOCKED`
from a real `CRITICAL` registry blocker in Alerts & Audit. This avoids a false failure
when Alerts & Audit independently samples a temporary read-only dependency timeout after
the current registry endpoint has already validated. CRITICAL registry failures remain fatal.
