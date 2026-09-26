# Step 17 — real Positions

Step 17 enables the second normal dashboard resource in real-provider mode: **Positions**.

## Canonical source

Read-only PostgreSQL query of the singleton `trading_runtime_state` row. The JSON `snapshot` is decoded through the verified `TradingStateSnapshot` contract.

Real facts exposed:

- `account_cash`;
- non-zero `account_positions` physical quantities;
- long/short side from the signed physical quantity;
- `strategies[].virtual_positions` grouped by strategy id;
- PostgreSQL `updated_at` persistence timestamp.

## Deliberately unavailable

The runtime snapshot alone does not canonically provide entry price, current mark, unrealized PnL, approved target weight/quantity or latest exchange reconciliation result. Step 17 therefore renders those fields as unavailable and uses `UNKNOWN` alignment instead of inventing `ALIGNED`.

No write/control path was added. Browser access remains API-only and all PostgreSQL queries are bounded/read-only.

## Expected result with the currently observed runtime snapshot

The user reported `positionCount: 0`, `accountCash: 100000`, `strategyCount: 1`. With that state, the real Positions page should show:

- Active Positions: `0`;
- Account Cash: `$100000.00`;
- empty positions table;
- valuation/PnL unavailable.

This is correct real-state behavior, not a missing mock fixture.

## Next

Step 18: real Execution view from current tracked orders plus `trading_fills`.
