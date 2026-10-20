# Chain: the PAPER trading chain end to end

Scope: the real service binaries wired as on the VPS PAPER chain, fed by the historical
feeder instead of Binance:

```text
historical_market_data_service (CSV, business time) -> SQLite + MarketDataUpdated + open(T+1)
  -> strategy_service -> portfolio_risk_service -> execution_state_service
  -> order_planner_service -> execution_state_service -> exchange_gateway (backend)
  -> simulated_exchange_service -> fills -> execution_state_service (accounting projection)
```

NATS JetStream and PostgreSQL are real (Docker). Business time runs accelerated through the
shared `ALGOTRADING_TIME_*` settings, one value for every service, as in
`deploy/historical_replay`. Configuration is the production PureRSI profile
(`config/strategies/pure_rsi_signal.json`: RSI(7), entry > 80, exit < 70, top-20 liquidity by
SMA(volume, 25)) and `config/portfolio/pure_rsi_equal_weight.json` (10% of equity per full
signal, entry/exit-only rebalancing).

## Synthetic market (worked by hand)

Three assets, daily bars from 2025-01-01, constant volume 1,000,000 so all are in the
liquidity universe once SMA(volume, 25) exists. Each close is the previous close plus a
change; open(T) = close(T-1) + 0.5 (a visible gap, so open(T+1) and close(T) differ).

| Asset | Days 1..J-1 | Day J (J = 32) | Days J+1..D-1 (D = 40) | Day D | After D |
| --- | --- | --- | --- | --- | --- |
| `RUNUP` | alternate +1 / -1 | +30 | +1 each day | +1 | +1 each day |
| `SWING` | alternate +1 / -1 | +30 | +1 each day | -30 | +1 each day |
| `SLIDE` | alternate -1 / +1 | -30 | -1 each day | -1 | -1 each day |

Business time starts at 00:00 UTC of day 31 (= `BUSINESS_START_DAY`): days 1..30 are
history already visible when the services start, as in production where the market
database holds the warm-up window before the strategy runs (the strategy refuses to start
its first cycle without data through the day before it, its bootstrap day).

RSI(7) before day J: three or four +1 and three or four -1 in any 7-day window, so
RSI is about 50 (< 80) under Wilder smoothing or a simple average: no entry.

Day J: average gain >= (3 x 1 + 30) / 7 = 4.71, average loss <= 4 / 7 = 0.57, so
RSI >= 89 > 80 for `RUNUP` and `SWING` under either RSI variant. `SLIDE` gets RSI near 0.

Days after J: every change is +1, so RSI stays above 80 for `RUNUP`, and for `SWING` until
day D. Day D for `SWING`: in the last 7 changes six are +1 and one is -30:
RSI = 100 - 100 / (1 + 6/30) = 16.7 < 70: exit. `RUNUP` keeps rising: held to the end.

## Rules

- C1 Entry timing: `RUNUP` and `SWING` are bought exactly once each, by a fill whose price
  is open(J+1) (decided at close(J), executed at the next open). Nothing is bought earlier.
- C2 Exit timing: `SWING` is sold by fills at open(D+1) and ends flat (quantity 0, a
  closed position row). `RUNUP` is still held at the end.
- C3 Never traded: `SLIDE` has no fills.
- C4 Sizing: each entry's notional (quantity x fill price) is 10% of account equity at
  the decision (equity is the initial 100,000 USD when the entries happen, nothing else
  is held), within 1% for the close(T) vs open(T+1) price difference.
- C5 Cash accounting: final cash = initial cash - sum(buy quantity x price) + sum(sell
  quantity x price) - sum(commission), and the commission of each fill is
  quantity x price x commission rate.
- C6 Realized PnL: the closed `SWING` position's gross PnL equals
  (exit price - entry price) x quantity.
- C7 One equity row per business day; the last day's equity = cash + RUNUP quantity x
  close(last day).
- C8 Reconciliation is CLEAN on every recorded day (our ledger equals the simulated venue).
- C9 Crash and restart: killing execution-state, strategy and the simulated exchange with
  SIGKILL in the middle of the run (between J and D) and restarting them yields exactly
  the same fills (count, prices, quantities) as an uninterrupted run; no fill is applied
  twice.

- C10 Liveness: if execution state is down long enough that a day's account snapshot
  arrives late (40 s of real time here, two to three business days), the chain delays
  that day's decision instead of losing it, and finishes every day with the same
  holdings as without the outage (RUNUP held, SWING sold, SLIDE never traded).

The business day lasts 15 s of real time: the chain needs about 6 s per day, and a
faster clock makes it fall behind, which itself triggers finding F1.

## Findings

F1 (C10, a late account snapshot stops the chain for good). Portfolio risk answers
"strategy intents for day T, but no account snapshot for T yet" with `Retry`
(`portfolio_risk_service_main.cpp`, "wait for the account snapshot"). Every consumer is
created with `max_deliver = 20` and redelivery is about one second apart, so after about
20 s JetStream stops delivering those intents. When the snapshot then arrives, nothing
makes the decision for day T; execution never completes cycle T, so the snapshot of T+1
is never published and every later day waits too. Observed in a run at 4 s per business
day: the intents of 2025-02-05 were retried 20 times from 17:20:56.4 to 17:21:15.4, the
snapshot arrived at 17:21:16.2, and no service processed a decision after that while the
feeder and strategy continued to 2025-02-14. In LIVE the same happens when the snapshot
of a day is more than about 20 s late (slow venue fills or reconciliation, an execution
restart around midnight). Retained test: `DISABLED_C10_LateAccountSnapshotDelaysButDoesNotStopTrading`.
Fixed (2026-10-19): every trading-service consumer now has no delivery limit
(`DurableConsumerOptions::UNLIMITED_DELIVERIES`), and `JetStreamBus` raises the limit of a
durable created earlier (the VPS consumers have 20) when the service binds it; a message is
dropped only by `Terminate` (transport R21). Test enabled as `C10_...`.

