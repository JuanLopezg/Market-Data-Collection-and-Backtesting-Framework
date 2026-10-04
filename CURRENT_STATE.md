# CURRENT STATE — Canonical replay / dashboard track

Last synchronized: 2026-10-03.

This file is the short source of truth for the active workstream. Older Txx/Stepxx documents remain useful historical evidence, but they do not override the behavior recorded here.

## Canonical public interface

```bash
python3 research/replay.py {fast|system|dashboard}
```

`fast` is the legacy research Backtester/PureRSI path. `system` runs the modern Strategy/Risk/Planner/venue-adapter/MOCK path. `dashboard` uses the same economics as `system` and additionally publishes dashboard state.

## RealTest acceptance contract

Acceptance source:

```text
storage/backtests/final_tests/pureRSI.csv
```

Current `realtest-parity` economics:

- preserve original historical OHLCV;
- decision/target notional on CLOSE(T);
- resolve target quantity using OPEN(T+1);
- full next-open execution;
- no volume-capacity limit for parity execution;
- zero slippage;
- zero commission/fees;
- direct trade comparison against RealTest;
- all mismatches are shown to the user;
- no hard-coded mismatch whitelist or T19-prefix acceptance.

The normal MOCK profile remains separate and retains its venue realism. Do not copy RealTest-parity friction settings into normal MOCK/live behavior.

## Accepted local evidence

### 107-day canonical baseline

```text
window                 : 2020-01-01..2020-04-16
candidate trades       : 25
fully matched          : 25
differences            : 0
canonical fills        : 50
fullRunFingerprint     : 94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2
```

Validated properties:

- system speed 250 / 1500 / 5000 -> identical fingerprint;
- repeated system runs -> identical fingerprint;
- system stop/checkpoint at day 50 + resume -> identical final fingerprint;
- dashboard economics -> identical fingerprint and byte-identical fills vs system;
- dashboard stop/checkpoint at day 50 + resume -> identical final fingerprint.

### Full-history canonical baseline

```text
window                 : 2020-01-01..2025-10-13
source days             : 2113
RealTest trades         : 631
candidate trades        : 631
fully matched           : 628
differences shown       : 4
canonical fills         : 1261
fullRunFingerprint     : 1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0
```

Validated properties:

- system full-history speed 250 / 1500 / 5000 -> identical fingerprint;
- dashboard full-history -> same fingerprint as system;
- dashboard full-history fills -> byte-identical to system fills.

The four visible RealTest differences are historical comparison differences, not code exceptions:

1. BNB: candidate exits one day earlier than the RealTest row.
2. FET: one RealTest trade has no candidate entry-date+coin match.
3. ZEC: final trade representation differs in exit time/state at the dataset boundary.
4. FET: one candidate trade has no RealTest entry-date+coin match.

## Pacing evidence

Dashboard pacing was exercised over a warmed full-prefix replay around 2024-06-05. Speed 250 and 5000 produced identical fills and fingerprint through 2024-06-10:

```text
fullRunFingerprint=b85026fe0cf5206dc4120af9b3a12cdc6a2a978d08faf69b0dc99b602bbe9024
canonicalFills=1004
```

A long slow dashboard run for visual acceptance is intentionally deferred until the final visual review.

## Restart/resume

`system` and `dashboard` support safe day-boundary replay checkpoints through the public Python CLI:

```text
--checkpoint-every N
--stop-after-days N
--resume
```

Resume uses the same explicit `--label` and durable run directory. The validated 107-day restart test stopped after day 50 and resumed to the exact continuous-run fingerprint.

## Window semantics

`--days N` selects exactly N available source days beginning at `--start`.

`--start` is the actual simulation start as well as the RealTest comparison-window start. It does not preload strategy history from before that date. Therefore a mid-history `--start` may legitimately produce different/no signals because rolling strategy state starts cold.

Use `--pace-start` / `--pace-end` when the goal is to retain the full warm-up/history but slow only a later interval.

## Dashboard state

Confirmed in the active code:

- dashboard mode is economically identical to system mode for the validated campaigns;
- pacing does not change validated fills;
- restart/resume works in dashboard mode;
- LedgerStatus appends each projected FILL row once; the previously suspected duplicate append is not present in the current tree;
- `--no-dashboard-up` runs dashboard publication without starting the web stack;
- without `--no-dashboard-up`, the local UI target is `http://localhost:8080`.

## Active validation entry point

```bash
bash validation/step58a_realtest_equivalence_implementation_gate.sh
```

This compatibility gate validates the active direct-RealTest config/docs and delegates to `validation/step58a_canonical_replay_suite_gate.sh` for behavioral checks.

Latest local result in this workstream: PASS.

## Intentionally retained historical material

Do not delete old T19/distributed replay logs, gates, or artifacts merely because they are no longer the active RealTest acceptance path. They are historical evidence. Likewise, generic MOCK synthetic-liquidity documentation is separate from `realtest-parity` and may remain where it accurately describes normal MOCK behavior.

## Next work

1. Keep the final long/slow visual dashboard acceptance run for the end, as planned.
2. Add/finalize compact release gates that cover the newly validated deterministic/restart/dashboard properties without requiring unnecessary multi-hour runs.
3. Close the current replay/dashboard acceptance phase and prepare the VPS replay deployment.
4. Verify local vs VPS deterministic equivalence.
5. Revisit current Hyperliquid API/adapter parity before any private TESTNET work.
6. Continue with private TESTNET, dashboard+venue validation, shadow mode, MAINNET hardening, and controlled LIVE readiness only after the preceding gates pass.
