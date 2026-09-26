# Step 45 — Live vs Expected REAL Projection

Step 45 upgrades the Step 25 rolling baseline into an explicit versioned dashboard contract without fabricating unsupported performance/execution history.

## Contract

- `contractVersion = step45-v1`
- `status = VALIDATED_LIMITED` when the REAL read-only contract is structurally usable
- `projectionReady = false` with `overallClassification = INSUFFICIENT_DATA` is a valid fail-closed state while fewer than five independent baseline observations exist
- latest completed canonical market day is the observation
- latest observation is excluded from baseline statistics
- baseline identity is SHA-256 fingerprinted from configuration + ordered canonical observations
- five currently supportable real metrics remain: entry-qualified assets, universe turnover, median RSI, RSI>=70 share, and top-5 liquidity concentration
- anomaly rows are derived exactly from non-NORMAL metric classifications

## Honest coverage

Coverage now:
- canonical market / strategy-input source is REAL and read-only
- `VALIDATED` once enough completed history exists for classification
- `INSUFFICIENT_DATA` (not fabricated PASS/FAIL) while history is still too short

Explicitly deferred:
- execution slippage/latency/reject historical distributions
- durable realized/unrealized PnL, equity and holding-time distributions
- accepted full-replay behavioural baseline

Step 42 explicitly does not provide durable PnL/equity, so Step 45 must not infer it. The future MockExchangeAdapter path must publish the same canonical evidence contract before accepted replay baselines can be compared here.

## Safety

The endpoint is authenticated GET-only. Classifications are observability signals, not trading instructions. Private auth, signing, submit/cancel, order routing and capital movement remain absent/deferred. Manual routing remains disabled.
