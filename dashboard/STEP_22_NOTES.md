# Step 22 — Real Market Data

Step 22 enables `/api/market-data` in real-provider mode.

## Canonical sources

- SQLite `market_volume_rank_daily` — latest persisted Binance ranking frontier.
- SQLite `ohlcv_data` — bounded canonical daily OHLCV history.
- PostgreSQL `strategy_market_update_checkpoint` — exact durable `MarketDataUpdated` + `StrategyIntentBatch` pair.

## Strategy parity

The dashboard recomputes diagnostics with the audited current PureRSI contract:

- canonical market top-N: `MARKET_TOP_N` (default 50);
- bounded strategy history: `MARKET_WARMUP_DAYS` (default 100 calendar days);
- universe: top 20 by SMA Volume(25), descending;
- rank/entry indicator: RSI(7), Wilder smoothing identical to `indicator_calculators.cpp`;
- entry: RSI(7) > 80;
- exit: RSI(7) < 70;
- max persistent signals: 10;
- LEVEL semantics.

Durable StrategyIntent state is joined only when its checkpoint timestamp is exactly the same as the SQLite ranking frontier. The dashboard does not mix cycles.

## Read-only boundary

`dashboard-api` uses the distro `sqlite3` client with `-readonly`. The canonical database is mounted read-only. No SQLite mutation statement is issued by the dashboard.

## Integrity

The page reports:

- target-day freshness for selected universe assets;
- missing daily candles and gap runs over the recent 25-calendar-day liquidity window;
- duplicate `(pair,date)` rows;
- invalid OHLCV rows;
- recomputed SMA Volume(25) and RSI(7);
- aligned durable PureRSI signal state when available.

Candidate diagnostics are explanatory dashboard projections; a first-class persisted candidate-rejection contract still does not exist.

## v0.22.1 compatibility fix
Live SQLite may use WAL/shared-memory coordination. The real overlay now permits
sidecar bookkeeping while over-mounting `database.db` read-only. The CLI reader
continues to use `-readonly` and `PRAGMA query_only=ON`.
