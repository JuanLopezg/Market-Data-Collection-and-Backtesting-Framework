# Step58A — Canonical Full-History RealTest Parity

Step58A is now owned by the single canonical replay CLI:

```bash
python3 research/replay.py {fast|system|dashboard} ...
```

The acceptance source is `storage/backtests/final_tests/pureRSI.csv`. Historical helper
runners and archived outputs remain evidence only; they are not acceptance inputs for the
current replay.

## RealTest-parity contract

`realtest-parity` keeps the production-style full-system path while reproducing the
historical research execution semantics needed for an apples-to-apples comparison:

`PureRSI -> StrategySignalEngine -> PortfolioRiskEngine -> NotionalOrderPlanner -> CanonicalVenueAdapter -> MOCK`

The contract is:

- strategy input uses the original historical OHLCV unchanged;
- the strategy/risk decision is made from `close(T)`;
- risk fixes the monetary target at `close(T)`;
- at `open(T+1)`, target quantity is resolved as `target_notional_usd / open(T+1)`;
- a FLAT closes the actually held parity quantity;
- execution is full at the historical next open;
- historical volume is not an execution-capacity limit;
- slippage is zero;
- matching fee is zero;
- accounting fee is zero;
- normal partial fills are disabled for this profile;
- the parity economic mirror preserves the historical Backtester arithmetic so venue-grid
  rounding cannot drift later sizing;
- the canonical MOCK path still receives and reconciles rule-conforming venue activity so
  adapter/accounting/recovery/reconciliation plumbing remains exercised.

This profile does not change `mock-default`. Normal MOCK keeps its venue-simulation
capacity, participation, slippage, grid, fee, accounting, reconciliation and ledger
behavior.

## Direct RealTest acceptance

Every canonical run compares candidate trades directly with
`storage/backtests/final_tests/pureRSI.csv` for the selected `[start, end]` window.
No mismatch identity is encoded as an accepted exception in the comparator.

Differences are displayed one-by-one. Full-history acceptance is a human decision after
all differences have been shown. For a partial cutoff, a reference trade that extends
past the cutoff is compared entry-only while it remains open in the candidate.

The currently accepted full-history canonical evidence is:

- window: `2020-01-01..2025-10-13`;
- candidate trades: `631`;
- fully matched trades: `628`;
- manual-review differences: `4`;
- canonical fills: `1261`;
- full-run fingerprint: `1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0`.

The four differences are review output, not coded acceptance rules.

## Speed invariance

Replay speed and pacing are scheduling concerns only. They must not alter trades, fills,
quantities, prices, PnL or the full-run fingerprint.

Full-history `system` runs at speeds `250`, `1500` and `5000` have the same accepted
fingerprint shown above. A paced dashboard segment was also verified at different speeds
with byte-identical `fills.csv` output.

## Dashboard parity

`dashboard` uses the same economics as `system`; it only publishes the visual read model.
The accepted full-history dashboard run produced the same `1261` fills and the same
fingerprint as `system`, and the two `fills.csv` files compared byte-for-byte equal.

## Safe restart / resume

Both `system` and `dashboard` support safe day-boundary checkpoint/restart through the
canonical CLI. The durable venue checkpoint is restored and the in-memory strategy/risk/
planner state is deterministically rebuilt to the same historical boundary before
continuing.

The accepted 107-day uninterrupted/restarted fingerprint is:

`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`

A resumed run must finish with the same fingerprint as the corresponding uninterrupted
run.

## `--start` and warm-up

The RealTest comparator respects both selected boundaries: references are restricted to
`[start, end]`. Starting the replay late, however, also starts strategy state cold unless
prior history is deliberately replayed. For tests that need mature indicators/universe
state, start from the normal historical beginning and pace only the target date range.

## Active validation

The active behavioral gate is:

```bash
bash validation/step58a_canonical_replay_suite_gate.sh
```

`validation/step58a_realtest_equivalence_implementation_gate.sh` remains as a compatibility
entry point and delegates to that canonical suite after checking that the active config and
documentation contain the current parity semantics.
