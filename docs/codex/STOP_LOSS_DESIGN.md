# LIVE long stop-loss: design and parallel tasks (2026-10-17)

User decisions (2026-10-17): every LIVE long position gets a **real venue stop-loss 30% below
the latest completed daily close**, **moved at every daily close** to 70% of that close (up or
down: it always sits 30% below the last close, it is not a ratchet). It lives on Kraken
(protects even when the VPS is down), is a **reduce-only stop-market SELL triggered by the mark
price**, follows the position size when it changes, and is cancelled when the position closes.
If it triggers and the strategy signal is still active, the strategy may **re-enter** at the
next daily cycle (no block).

Kraken Futures has no attached SL/bracket in `sendorder`: the stop is a separate `orderType=stp`
order with `stopPrice`, `reduceOnly=true`, `triggerSignal=mark`. A reduce-only stop can never
open or increase a position; it is not cancelled automatically when the position closes.

## Contract (task 0, done and tested)

- `ExecutionOrder::position_stop_price` (`lib/src/execution/execution_order.h`): `> 0` makes the
  order the resting protective stop of the strategy's long position in `coin`. It is a SELL,
  quantity in source units, `position_stop_price` in USD per source unit. `isPositionStop()`.
  `validateConditional()` rejects a position stop that is a BUY, carries research bracket fields
  (`entry_stop_price`, `protective_stop_price`, `parent_order_id`) or `fill_at_reference_price`.
- Service wire (`lib/src/transport/message_json.cpp`): optional key `"position_stop_price"` in the
  order JSON of `SubmitOrderCommand` (absent = 0, so every existing message is unchanged).
- Durable state (`lib/src/persistence/postgres_state_store.cpp`): the same optional key per order.
- `reference_price` of a position stop = the latest close used when it was created (the gateway's
  USD-value check keeps working unchanged); `active_from = created_at` (sent at once).
- Temporary guards until the tasks below land: the live gateway rejects a position stop locally
  (`POSITION_STOP_UNSUPPORTED`) and the simulated exchange rejects it, so it can never be sent or
  filled as a market sell.
- Test: `validation/position_stop_contract_test.cpp` (in `validation/mock_component_tests.sh`).

## Semantics every task follows

- One position stop per (strategy, coin) while the strategy holds a long. Quantity = the
  strategy's whole position; price = the latest completed daily close of the coin (Binance
  close(T), source units) x (1 - 0.30). Shorts get none (the chain is long-only today).
- Execution owns the lifecycle:
  - after a fill that opens or changes a long: a stop at 70% of the latest close (for a new
    entry that is the decision close T), for the new quantity;
  - at every daily close T (when the new closes are known, before/with the day's plan): every
    held long gets its stop moved to 70% of close(T), whether that is higher or lower;
  - when the position goes flat (strategy exit): cancel its stop.
  - A triggered stop is an ordinary fill of a known order: it reduces the position, the stop is
    Filled, and no new stop is created until a new entry.
- **Replacement order (moving or resizing a stop): submit the new stop first and cancel the old
  one only after the new one is Accepted at the venue**, so the position is never unprotected.
  Two reduce-only stops for the same position may coexist for a moment; reduce-only keeps them
  from ever selling more than the position. If the new stop is rejected, keep the old one and
  raise an alert.
- A position stop is **not** a pending trade: the order planner ignores it for current/pending
  quantities and never cancels it; the daily cycle completion, the scheduled reconciliation and
  Manual Control must not wait for it to finish (it stays open across days).
- The venue stop price is rounded **down** to the instrument tick; the venue size is the
  position size rounded down to the step (as market orders today).
- Expected volume: about one replacement (one new stop + one cancel) per held position per day,
  well inside Kraken's rate limits.

## Status

- Task 0 (contract): done. Task D (execution lifecycle, planner exclusion, gating, chain
  configuration): done locally, tested (`validation/position_stops_test.cpp`).
- Tasks A, B, C (Codex): done and integrated. Deployed 2026-10-17 (release 20261017_190518,
  `LIVE_LONG_STOP_FRACTION=0.30`); first stops accepted by Kraken at 23:14 UTC.

## Parallel tasks and file ownership

Each task edits only its own files. Nobody edits `CURRENT_STATE.md`, `docs/ROADMAP.md`,
`deploy/chain/README.md` or `validation/mock_component_tests.sh`: report the evidence and the
test line to add; Claude integrates docs, scripts and the release. No deploy, no VPS access,
no commit/push, no real orders.

| Task | Owner | Files |
| --- | --- | --- |
| A. Kraken gateway: send, track, cancel and fill resting stop orders | Codex | `live_trading/exchange_gateway/**` |
| B. Simulated exchange: resting position stops for PAPER/tests | Codex | `lib/src/exchange/adapters/simulated_exchange.h`, `live_trading/simulated_exchange_service/**`, new `validation/position_stop_simulated_exchange_test.cpp` |
| C. Dashboard: show each position's stop-loss | Codex | `dashboard/**` |
| D. Execution lifecycle, planner, cycle/reconciliation/manual gating, configuration | Claude | `lib/src/execution/**` (except `execution_order.h`), `lib/src/recovery/**`, `live_trading/execution_state_service/**`, `deploy/chain/**`, `config/**` |

Configuration (task D): `LIVE_LONG_STOP_FRACTION=0.30` in `chain.env` (0 = off), passed to the
execution service only; PAPER keeps 0 unless set.
