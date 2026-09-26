# Step 36 — Public Venue Trading Rules / Precision

Status of implementation: **READY FOR LOCAL RUNTIME GATE; NOT YET ACCEPTED UNTIL USER OBSERVES PASS**.

## Goal

Validate the public Hyperliquid TESTNET trading constraints needed before a future private order path can be built. Step 36 operates only on symbols that Step 35 classified as `VALID`/supported.

It does **not** enable private auth, signing, balances, submit, cancel, or real capital.

## Public inputs

The dashboard calls only the existing Hyperliquid TESTNET `/info` endpoint with:

- `{"type":"meta"}`
- `{"type":"allMids"}`

For each Step 35-supported symbol the read model validates and exposes:

- exact internal -> venue symbol identity from the explicit Step 35 manifest;
- `szDecimals` from current public perp metadata;
- exact size step derived as `10^-szDecimals`;
- `maxLeverage` from current public metadata;
- `onlyIsolated` / `marginMode` when present;
- `isDelisted` and fail-closed behaviour if true;
- current public mid;
- Hyperliquid perp price rule: at most 5 significant figures and at most `6 - szDecimals` decimal places, with integer prices always allowed;
- documented minimum order notional of 10 USD/USDC-equivalent notional;
- a read-only estimated minimum quantity at the current mid, rounded **up** to the venue size precision.

The estimated minimum quantity is diagnostic only. It is not an order-planner rounding implementation and is never used to submit an order in Step 36.

## Fail-closed rules

Step 36 is BLOCKED if any Step 35-supported mapping:

- disappears from the current public rule snapshot;
- has invalid `szDecimals`;
- has no positive `maxLeverage`;
- is marked delisted;
- has no valid positive public mid;
- cannot produce a finite rounded minimum-size estimate;
- produces a rounded estimate below the 10 USD minimum notional.

Step 35-unsupported or venue-absent assets are not promoted into the supported set. They remain explicitly non-routable.

## API

Authenticated read-only endpoint:

`GET /api/venue-rules`

No mutating method is registered.

## UI

Infrastructure adds:

`Step 36 · Public Trading Rules / Precision`

It shows coverage plus per-supported-symbol precision/leverage/margin/min-size diagnostics.

## Public protocol references audited for this step

- https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/tick-and-lot-size
- https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/info-endpoint/perpetuals
- https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/error-responses

The 10 USD minimum order notional is treated as an explicit protocol constant and must be revalidated again before private order submission and before the final production venue is enabled.

## Acceptance

Run:

`./scripts/step36-venue-rules.sh`

Expected final line:

`STEP 36: PASS — PUBLIC VENUE TRADING RULES VALIDATED`

Do not start Step 37 until the user observes that PASS on the local real stack.
