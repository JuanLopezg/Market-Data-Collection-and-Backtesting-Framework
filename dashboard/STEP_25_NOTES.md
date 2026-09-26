# Step 25 — Real Live vs Expected baseline

Step 25 enables `/api/live-vs-expected` in real-provider mode without fabricating historical PnL or execution distributions.

## Canonical source

The baseline is reconstructed read-only from the same canonical SQLite market database used by Market Data:

- `market_volume_rank_daily`
- `ohlcv_data`

The dashboard recomputes the verified strategy inputs over a bounded rolling window:

- top-N by SMA Volume(25) inside the canonical exchange top-N;
- RSI(7) using Wilder smoothing;
- latest observation excluded from baseline statistics;
- p10–p90 baseline envelope;
- sample standard deviation and z-score classification.

## REAL metrics in this step

- Entry-qualified assets: top-20 assets with RSI(7) > 80.
- Universe Turnover: percentage of top-20 membership changed from the previous completed day.
- Top-20 Median RSI.
- RSI >= 70 Share.
- Top-5 Liquidity Concentration inside the reconstructed top-20.

## Explicit non-claims

Step 25 does **not** invent or infer:

- accepted replay PnL distributions;
- realized/unrealized PnL behaviour;
- holding-time distributions;
- slippage distributions;
- fill-latency distributions;
- reject/retry historical baselines.

Those need versioned replay/ledger/execution baseline artifacts and remain future work.

The page therefore describes itself as a **canonical SQLite rolling baseline**, not the final accepted-replay behavioural baseline.
