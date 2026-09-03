# Distributed vs fast comparator

`distributed_fast_compare.cpp` is a diagnostic utility for the distributed replay.
It reads retained runtime events from JetStream and independently replays the same
released market slices through the in-process engines, reporting the first divergence
by timestamp and layer.

Compared:
- strategy signals,
- `DecisionBatch` actions, target weights, and reference capital,
- submit orders,
- fills,
- account cash and filled positions,
- per-strategy virtual positions,
- closed trades and cumulative closed PnL reconstructed from fills.

Transport metadata (`message_id`, correlation IDs, durable delivery details) is not
part of the economic comparison.

## Validated PureRSI profiles

The strategy side is identical in both modes:
- PureRSI RSI(7), entry 80, exit 70,
- max 10 active signals,
- Top20 liquidity universe using SMA(volume,25),
- RSI ranking,
- strategy allocation 100%,
- gross leverage cap 1.50,
- asset cap 1.50.

`--portfolio-mode equal-weight` (default):
- EqualWeightSizer 10% per full signal,
- EntryExitOnly rebalance.

`--portfolio-mode vol-target`:
- SampleCovarianceEstimator lookback 5,
- 365 periods/year,
- target annualized volatility 20%,
- Threshold rebalance 2%.

The distributed harness maps the same mode to the matching portfolio-risk JSON
configuration, so one argument selects both sides of the comparison.

Useful flags:
- `--expected-cycles N` (required)
- `--nats-url nats://...`
- `--initial-cash 100000`
- `--commission-rate 0`
- `--portfolio-mode equal-weight|vol-target`
- `--require-trading` fails if the replay did not produce at least one order, fill,
  and closed trade, preventing a trivial all-flat comparison.

On mismatch the process exits non-zero with a message such as:

`first divergence @ 20240131 [decision]: BTC action/weight mismatch ...`

## RealTest external-reference bridge (PATCH 32)

The historical harness can optionally pass the distributed trade history through the **same** RealTest policy used by `research`:

```bash
tools/distributed_compare/run_historical_compare.sh \
  ... \
  --realtest-csv storage/backtests/final_tests/pureRSI.csv
```

The comparator links `research/src/realtest.cpp` directly and invokes `compareBacktestBySizing(...)`. It does not duplicate or loosen the RealTest rules. See `validation_32A/` and `validation_32B/` for full-history gates.
