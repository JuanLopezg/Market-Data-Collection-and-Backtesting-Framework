# Validation guide

Run checks from the repository root under WSL. Current source and test output take
precedence over historical step notes. [CURRENT_STATE.md](../CURRENT_STATE.md)
records the last validated results and remaining acceptance work.

## Default release gate

For meaningful library, canonical replay, execution, exchange, accounting, recovery
or cross-cutting changes:

```bash
bash validation/step59_canonical_replay_release_gate.sh
```

This builds the active canonical runner and checks 107 historical days, direct
RealTest comparison, deterministic rerun, system/dashboard equality, safe restart/resume
in both modes and paced-speed invariance. No Docker/browser or full-history replay is
required. Expected result: 25 matched campaigns, zero differences, 50 fills.

Expected compact fingerprint:

```text
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2
```

A changed fingerprint is a regression until explained. RealTest discrepancies are
compared directly with `storage/backtests/final_tests/pureRSI.csv`; historical
mismatch identities are not an acceptance whitelist.

Build the project separately when changing compiled sources/build definitions:

```bash
meson setup build --reconfigure
meson compile -C build
```

Use `meson setup build` for a first configuration. Do not run full-history or
slow browser tests for routine readability changes.

## Current time, data and service checks

| Check | Purpose |
| --- | --- |
| `time_handler_unit_test.sh` | Business time, day/visibility boundaries and technical-time behavior. |
| `time_handler_factory_unit_test.sh` | Shared time environment parsing/configuration. |
| `historical_replay_time_env_test.sh` | Generated per-run reference time settings. |
| `canonical_market_data_no_lookahead_test.sh` | Canonical history access cannot expose future candles. |
| `historical_visibility_no_lookahead_test.sh` | Historical source visibility boundaries. |
| `historical_market_data_source_unit_test.sh` | Historical CSV source behavior. |
| `live_pre_exchange_e2e_audit.sh` | Combined current live source/deployment invariants: daily gating, SQLite ownership, identity and topology. |
| `no_legacy_clock_audit.sh` | Current source/build/deployment excludes the removed shared-clock control plane. |
| `historical_replay_topology_audit.sh` | Isolated historical Compose structure, stream/port boundaries and read-only/query-only consumer ownership (volume access allows WAL sidecars). |
| `historical_replay_no_shared_clock_audit.sh` | Historical services use the current TimeHandler architecture. |
| `historical_market_data_feeder_audit.sh` | Commit/publication and historical visibility source invariants. |
| `distributed_anti_lookahead_audit.sh` | Distributed source visibility/causality boundaries. |
| `time_usage_audit.sh` | Diagnostic inventory of time usage; inspect output rather than treating a search count as economic acceptance. |

Example:

```bash
bash validation/live_pre_exchange_e2e_audit.sh .
bash validation/market_data_time_handler_audit.sh .
bash validation/no_legacy_clock_audit.sh .
bash validation/historical_visibility_no_lookahead_test.sh .
```

Individual `*_time_handler_audit.sh` checks cover strategy, risk, planner,
execution state, gateway, simulated exchange and market data. Individual
`live_*_audit.sh` files isolate contract identity, daily gates, database
ownership, runtime-clock rules and deployment topology when diagnosing a failure.

These source audits are not proof that a running broker/container stack survived
failures. `live_pre_exchange_runtime_acceptance.sh` is the separate runtime
campaign; read its requirements before running it. Historical Compose preparation
is documented in [deploy/historical_replay/README.md](../deploy/historical_replay/README.md).

## Retained component gates

Numbered names are retained because they identify accepted contracts/evidence and are
referenced by other gates. They are not the recommended reading order for the code.

| Gate family | Responsibility |
| --- | --- |
| `step47a` / `step47b` / `step47c` | Multi-venue boundaries, Hyperliquid semantics and portability specifications. |
| `step48` | Canonical multi-venue adapter interface. |
| `step49` | MOCK catalog and venue rules. |
| `step50` | Order admission and lifecycle. |
| `step51` | Deterministic matching and fills. |
| `step52` | Account, margin, positions and accounting. |
| `step53` | Snapshot/user-stream recovery. |
| `step54` | Reconciliation and ledger parity. |
| `step55` | Fault/chaos/rate-limit behavior. |
| `step56` | Full-system replay runtime. |
| `step56a` | Source mapping and replay acceptance foundation. |
| `step57` | Manual targets through the normal risk/planning/MOCK pipeline. |
| `step58` | Dashboard simulation publication/integration. |
| `step58a` | Canonical replay suite and compatibility/equivalence implementation checks. |
| `step59` | Current compact release gate. |

Each component gate lives beside its `*_test.cpp` or Python audit when applicable.
Use the relevant component gate for a behavior change; use Step59 for release evidence.
`docs/venue/step*/` files, registries and historical checksum manifests are inputs
to retained gates, so they remain intact.

Historical checksums describe their frozen source snapshot. Library renames may make
old paths/hashes differ from current source. Such drift must be reported and interpreted;
regenerating the manifest merely to turn a historical gate green is not validation.
Step59 tests current executable behavior independently of those historical hashes.

## Historical regression audits

Retained `t16_*` through `t21_*` audits document targeted issues such as
immutable bootstrap frontiers, event-time ordering, exact flattening, bounded fingerprints,
capacity, CSV comparisons and strategy catch-up/restart. Read each script before choosing
it: some expect historical run artifacts or a particular diagnostic runner. They are
not one aggregate current release suite.

`t17_t24_preflight.sh` is a historical distributed campaign preflight.
The duplicate old `t23_no_legacy_clock_audit.sh` was consolidated into
`no_legacy_clock_audit.sh`. Retained historical runner/preflight references now
use the current audit, which does not require a one-time cleanup-summary file.

Removed checks required the deleted shared Clock interfaces, replay-controller,
`deploy/distributed_replay/` or `tools/distributed_compare/` harness.
The obsolete combined restart/chaos/logging suites and their migration diaries were
removed with those dependencies. Current TimeHandler, canonical restart and component
chaos tests remain. Generated run evidence and frozen venue contracts were not deleted.

## Dashboard and analysis checks

Dashboard contracts/deployment have their own scripts under `dashboard/scripts/`.
See [dashboard/README.md](../dashboard/README.md) and its current API/source guides.

```bash
python3 dashboard/scripts/verify-project-contracts.py --project-root .
python3 tools/verify_tool.py
```

The second command verifies the retained sensitivity-report program/configuration;
it does not run studies or produce a report. See
[tools/README_sensitivity_report.md](../tools/README_sensitivity_report.md).
