# algoTrading

C++/Python crypto trading research and execution project with a canonical historical replay path, a live-shaped runtime stack, a MOCK venue, and an operations dashboard.

## Current source of truth

Read [`CURRENT_STATE.md`](CURRENT_STATE.md) first. It records the currently validated replay behavior, fingerprints, known RealTest differences, restart support, and the remaining work before TESTNET/LIVE.

The canonical research entry point is:

```bash
python3 research/replay.py {fast|system|dashboard} [options]
```

- `fast` runs the legacy research Backtester/PureRSI path.
- `system` runs the modern Strategy -> Risk -> Planner -> canonical venue adapter -> MOCK stack.
- `dashboard` runs the same system economics and additionally publishes dashboard state.

Use `python3 research/replay.py <mode> --help` for the current options.

## Canonical RealTest parity profile

`system` and `dashboard` currently use the `realtest-parity` profile for the canonical research comparison. Its purpose is to reproduce the historical Backtester execution economics while still traversing the modern runtime stack:

- original historical OHLCV is preserved;
- strategy/risk decide target notional on CLOSE(T);
- target quantity is resolved at OPEN(T+1);
- fills are full next-open fills;
- execution volume does not capacity-limit parity fills;
- slippage is zero;
- commission/fees are zero;
- `storage/backtests/final_tests/pureRSI.csv` is the acceptance source;
- no mismatch whitelist or hard-coded exception acceptance is used.

The normal MOCK profile remains separate and keeps its realistic venue mechanics. RealTest-parity settings are not a statement about future live venue behavior.

## Locally validated canonical results

Full history (`2020-01-01..2025-10-13`, 2113 source days):

```text
RealTest trades checked : 631
Candidate trades total  : 631
Fully matched           : 628
Differences             : 4
canonicalFills          : 1261
fullRunFingerprint      : 1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0
```

The four differences are surfaced for human review: the historical BNB exit-date difference, the RealTest FET trade with no candidate match, the final ZEC end-state/time difference, and the extra candidate FET trade. They are not encoded as accepted exceptions.

The 107-day deterministic baseline is:

```text
Candidate trades total : 25
Fully matched          : 25
Differences            : 0
canonicalFills         : 50
fullRunFingerprint     : 94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2
```

This 107-day fingerprint was reproduced across repeated runs and multiple TimeHandler speeds. Full-history `system` speed invariance was also verified, and full-history `dashboard` produced the same fingerprint and byte-identical `fills.csv` as `system`.

## Common commands

Short canonical system replay:

```bash
python3 research/replay.py system --days 107 --label system_107d
```

Full canonical system replay:

```bash
python3 research/replay.py system --label system_full
```

Dashboard replay without starting the web stack:

```bash
python3 research/replay.py dashboard --days 107 --label dashboard_107d --no-dashboard-up
```

Dashboard replay with the UI stack:

```bash
python3 research/replay.py dashboard --label dashboard_visual
# open http://localhost:8080
```

Safe day-boundary restart/resume is supported by both `system` and `dashboard`:

```bash
python3 research/replay.py system --days 107 --label restart_107d \
  --checkpoint-every 25 --stop-after-days 50

python3 research/replay.py system --days 107 --label restart_107d \
  --checkpoint-every 25 --resume
```

The same flags work in `dashboard` mode.

### About `--start`

`--start` defines the actual replay start and the RealTest comparison window start. Starting in the middle of history therefore also starts strategy state/warm-up there; it is not equivalent to fast-forwarding from 2020. For a warmed strategy plus pacing only around a later date, keep the original replay start and use `--pace-start/--pace-end`.

## Validation

The active Step58A compatibility entry point is:

```bash
bash validation/step58a_realtest_equivalence_implementation_gate.sh
```

It delegates behavioral proof to:

```bash
bash validation/step58a_canonical_replay_suite_gate.sh
```

The current gate checks direct RealTest ownership, the 107-day system comparison, original OHLCV, zero parity fees/slippage, no execution volume capacity in parity, the frozen normal-MOCK baseline, cutoff/open-trade semantics, and one canonical public replay CLI.

Older Txx/distributed validation assets remain in the repository as historical evidence and regression tooling; they are not the current RealTest acceptance path.

## Dashboard

`dashboard` is observational around the same replay economics as `system`. Full-history economic equality with `system`, pacing invariance, and day-boundary restart/resume have been locally validated. The long slow visual acceptance run is intentionally deferred until the final dashboard review.

See [`dashboard/README.md`](dashboard/README.md) for the dashboard-specific history and [`deploy/historical_replay/README.md`](deploy/historical_replay/README.md) for replay/deployment notes.

## Venue / live status

Private Hyperliquid integration and capital routing remain deferred. Public/read-only venue groundwork may exist in the repository, but the current accepted workstream is MOCK/replay-first. TESTNET, shadow mode, MAINNET hardening, and controlled LIVE readiness remain later phases.

## Build

The project contains Meson-based components and the canonical replay wrapper also builds the required C++ replay runner when needed. For the current replay workflow, prefer `research/replay.py` rather than invoking internal replay binaries directly.

## Handoff

Run:

```bash
./handoff.sh
```

The handoff package includes the major source directories plus root project state files, including `CURRENT_STATE.md`.

## Disclaimer

This project is for research and engineering purposes. It is not financial advice.
