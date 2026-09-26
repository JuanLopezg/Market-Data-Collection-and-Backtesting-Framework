# Step 20 fix v0.20.1

Fixes the real Pipeline HTTP 503 seen against the user's LIVE checkpoint.

The durable `account_payload.strategy_positions` is accepted in both verified historical shapes:

- object keyed by strategy id: `{ "1": { "BTC": 0.1 } }`
- planning-style array: `[{ "strategy_id": 1, "positions": { "BTC": 0.1 } }]`

Both are normalized read-only inside dashboard-api. No trading state is changed.

The unrelated `shell-status` 503 remains intentional until its real adapter is implemented.
