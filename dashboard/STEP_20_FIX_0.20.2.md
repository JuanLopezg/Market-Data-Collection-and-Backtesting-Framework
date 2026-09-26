# Step 20 fix v0.20.2

Fixes the real Pipeline decoder for the third `strategy_positions` encoding verified in the live PostgreSQL checkpoints.

Supported read-only encodings:

- object map: `{"1":{"BTCUSDT":0.1}}`
- planning object array: `[{"strategy_id":1,"positions":{"BTCUSDT":0.1}}]`
- account tuple array: `[[1,{"BTCUSDT":0.1}]]`

The tuple-array form is used by `portfolio_risk_live_account_checkpoint.payload` and by the account payload embedded in `portfolio_risk_live_decision_checkpoint`. The dashboard normalizes these representations only in its read model; no trading-side contract is changed.
