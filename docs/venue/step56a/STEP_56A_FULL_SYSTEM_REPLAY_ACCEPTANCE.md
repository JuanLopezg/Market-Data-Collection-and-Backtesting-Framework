# Step56A — Full-System Replay Acceptance Campaign

**Bound Step56 runtime fingerprint:** `79ec33ef33b9fb45410b1a10038a74dc0ac6b00f064e75175bb9330f2fddf27f`

Step56A runs the frozen real historical OHLCV source through the complete in-process production-engine chain:

`TimeHandler -> StrategyPureRSI -> StrategySignalEngine -> PortfolioRiskEngine -> NotionalOrderPlannerEngine -> CanonicalVenueAdapter -> MockExchangeAdapterV1 -> Fill -> Accounting -> Recovery -> Reconciliation/Ledger`

## Acceptance window

The fast acceptance gate intentionally uses **2020-01-01 through 2020-04-16** from the same historical file used by the previous replay program. The window contains **107 real daily slices and 3,401 OHLCV rows**.

This is **full-system**, not **full-history**: every trading stage is exercised, but the gate does not rerun the entire 2020-01-01 through 2025-10-13 dataset every time. A later full-history/determinism campaign can reuse the same runner.

Historical CSV SHA-256: `7b4a48df39eb8049fb9f05856b0de44d80e8d21d2c513c9c6eb8db74405ae381`.

## Explicit historical-source identity

The historical source uses symbols such as `BTC`, `ETH` and `ATOM`, while the canonical MOCK venue uses explicit assets such as `BTCUSDT`.

Step56A adds a frozen **150-row explicit mapping file**. The runner performs exact lookup only. There is no runtime suffix append/strip, case conversion, fuzzy alias or venue fallback.

Mapping SHA-256: `f83fe5ae3ebb128331bfcf97d678aa78d58138c7f2bb9b2f370502fcb7d69fa7`.

## PureRSI and portfolio semantics

The runner uses the production `StrategyPureRSI`, `StrategySignalEngine`, `PortfolioRiskEngine` and `NotionalOrderPlannerEngine` with the frozen configuration semantics:

- top 20 by SMA Volume(25);
- RSI(7), using the project's indicator implementation;
- entry RSI > 80;
- exit RSI < 70;
- persistent LEVEL signals;
- maximum 10 active signals;
- equal weight 10% per full signal;
- allocation 1.0;
- max gross leverage 1.50;
- max asset weight 1.50;
- `EntryExitOnlyRebalancePolicy`.

Strategy config SHA-256: `5749bef7d6adfc1aeee9d3d0adaa6ec05daa7d6f93c1c4cbee5bca6f7bcf407a`.  
Portfolio config SHA-256: `8cdcdee3b868ec2d7e23cb73eb2a4d15b2c9f8d3f14ca4aa29630f4a77e95ed2`.

## TimeHandler release gate

The real project `TimeHandler` is used to gate each historical open and close release. The acceptance speed is `100000000x`, so this gate stays fast.

The business timestamps supplied to Step56 remain deterministic:

- open: `YYYYMMDD*10+1`;
- close: `YYYYMMDD*10+9`.

Wall-clock duration can change how quickly the gate completes, but not prices, fills, accounting, ledger, reconciliation or run fingerprints.

## Clean-room baseline

The frozen clean-room run produced:

- planned submits: `50`;
- planned cancels: `0`;
- canonical fills: `50`;
- canonical accounting events: `50`;
- known orders: `50`;
- final open orders: `0`;
- final positions: `0`;
- final equity: `106407.267465490004`;
- reconciliation: `CLEAN` with `0` issues;
- economic fingerprint: `862ebbaf6ac97f2e`;
- stream fingerprint: `973c0e9124f20021`;
- ledger head: `25d7e82d07afef691de2044ef7019717b2c43bf7f903530bc2f15233e9bad04f`;
- chaos evidence fingerprint: `f8c85845b2585d7ac15c5f6e0595b43d3cdbfc6ed5e70d1ba0ac17fb491a8eb3`;
- full-run SHA-256: `5bf459d5a560172fad310416f9c0c749b0d2ff81ccbc88d44c814876c62f2bb4`.

The user's gate compares these deterministic outputs against the frozen baseline exactly (equity uses a tiny numeric tolerance only for serialization).

The Step56A fingerprint is **not expected to equal T24's old simulated-exchange fingerprint**. The multi-venue adapter, synthetic MOCK matcher and Step52 fee/accounting model are intentionally different. Step56A freezes the new pipeline's own evidence.

## Runtime output

The local gate writes:

`deploy/historical_replay/run/step56a_full_system_replay_summary.json`

The durable MOCK state used by the gate lives only in a temporary directory and is removed automatically.

## Next

After a real local PASS: **Step57 — Manual Control -> MockExchange normal pipeline**. Step58 then connects the replay read models to the dashboard so orders, fills, positions, PnL, ledger and reconciliation can be watched while the replay runs.
