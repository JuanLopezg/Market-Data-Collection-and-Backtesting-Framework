# Validation guide

`bash validation/quote_volume_test.sh` checks actual quote turnover, base-only schema
compatibility, additive backfill, ranking, transactional rejection, optional wire
fields and duplicate detection against the current WSL-built library. The bounded
`paper_trading_test.py` fixture also verifies completed quote values, excludes poisoned
unfinished turnover, and checks quote-volume dashboard signals plus restart accounting.
`PAPER_FIXTURE_BINARY` can reuse a previously compiled fixture data server only
when its adjacent `fixture.go` exactly matches the embedded source and the binary
has an ELF header. It does not reuse trading-service binaries. The default fixture
build allows 600 seconds for compilation on small VPS hosts.

The [paper stack guide](../deploy/paper_trading/README.md) describes
`python3 validation/paper_trading_test.py`: bounded internal-network public-API
fixture, actual downloader, simulated execution, dashboard telemetry and restart.
It does not run full history or complete VPS/overnight acceptance.

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

`python3 research/replay.py resources --days 100 --seconds 300` runs an isolated
canonical backtest alongside the service sandbox and dashboard, then writes an
offline CPU/RAM HTML report and stops its own stack. Setup is outside the timed
window. See [research/REPLAY.md](../research/REPLAY.md) for scope and limits.
`python3 validation/resource_profile_test.py` checks missing/stale resource evidence,
overlapping host/container scopes and safe failure-report rendering.

`python3 validation/local_service_campaign.py prepare` builds a separate current
service sandbox with bounded synthetic data and project-owned state. Start/check,
duplicate injection, service/infrastructure faults, checkpoint/outbox barriers,
container recreation, daily log collection and resource sampling are described in
the [local campaign runbook](LOCAL_SERVICE_CAMPAIGN.md). Preparation and local
proofs do not accept a VPS or private venue.

`python3 validation/transport_persistence_integration_test.py` compiles the current
JetStream/PostgreSQL adapters and runs them against disposable local containers
using cached `nats:2.10-alpine` and `postgres:16-alpine` images. Requires WSL/Linux,
Docker, a C++20 compiler, pkg-config, libnats/libpq and nlohmann JSON headers.
It checks actual SQL rollback of snapshot plus fill, a consumer-process crash
after commit but before ACK, abrupt broker/database restarts, wildcard durable
rebinding, incompatible-filter rejection, publish/audit deduplication and exact
cash/position/timestamp/next-order/fill-ID recovery. The fixture handler explicitly
owns economic deduplication; the state store alone does not prevent a caller from
applying the same cash/position delta twice. A second infrastructure restart checks
that the ACKed delivery stays consumed. Only UUID-owned, labeled containers and
their anonymous volumes are removed, including on failure; no LIVE stack is started.
Dummy credentials and compiled artifacts stay in temporary directories. This is
adapter integration, not the five-service LIVE pipeline, service outbox recovery,
container recreation, private routing or VPS acceptance. See the runtime acceptance
command below for the separate deployed-cycle check.

`python3 validation/daily_service_logs_test.py` exercises an isolated real rsyslog
receiver, UTC date templates, restart/append, credential filtering, two-day safe
cleanup, per-write size trimming, oversized-record handling and merged LIVE/dashboard
logging overlays. Requires rsyslog 8 with `omprog`, Python and
Docker Compose 2.24.4+ under WSL. It uses only temporary files and dummy configuration;
it does not deploy trading containers or install host services. Systemd units can
be checked with `systemd-analyze verify deploy/live/logging/*.service
deploy/live/logging/*.timer`. Actual Docker-to-host delivery and VPS acceptance
remain separate. Setup and limits are in the [logging guide](../deploy/live/README.md#vps-log-retention).

`python3 validation/ai_index_test.py` checks generated navigation exclusions,
including force-tracked credentials/ignored files, symlinks, ambiguous headers,
deterministic output and stale edits/additions/deletions. It uses isolated temporary
Git repositories without staging or modifying the real repository.
`python3 tools/generate_ai_index.py --check` checks the actual repository's three
local generated outputs. Refresh first when missing/stale; see the
[index guide](../docs/codex/AI_INDEX.md). These are navigation checks, not trading
validation.

`python3 validation/research_html_report_gate.py` builds the CURRENT research targets
and checks a bounded 107-day PureRSI study against the independently configured
CURRENT fast runner. It verifies CSV/HTML contracts, deterministic reruns,
account/PnL consistency, shorter-window causality, fees, invalid combinations,
open-cutoff/holding-bar metrics and all four CURRENT HTML strategy selections.
CSV columns and HTML sections are checked explicitly without a frozen runtime.
It also checks compact CURRENT XH/XH-ATR grids, prefixes and nonzero fees.
`research_stop_entry_test.cpp` covers trigger/gap matching, one-bar expiry,
missing-asset expiry, split-fill idempotency, cancellation, in-memory restoration,
unsupported adapter/wire/store rejection, deferred exits, cutoff and ATR ratchets.
This proves in-process research behavior, not durable/live conditional routing.
The same fixtures cover short gap sizing, fill-sized protective covers,
bullish/bearish entry-bar ordering, later protective gaps, bracket cancellation
before timed market exit, duplicate terminal events and observed holding periods.
The gate also checks seven CURRENT initial baselines, 82 compact combinations,
enumerates the original 37,702 valid full-grid points without running them,
reconciles zero/nonzero-fee accounts and verifies deterministic reports, prefixes,
PureRSI HTML equality and logged statistics parity.
The BTC moving-average consumer is checked with all 78 SMA combinations over
107 days, compact endpoints, exact baseline/fee parity with the initial study,
deterministic CSV/HTML, shorter-window prefixes, original metadata counters and
injected invalid/failed runs with reasons and empty metric cells.
The gate also builds the CURRENT XH robustness executable and checks its distinct
full ranges, 16 compact runs, baseline/fee exports against the HTML runner,
deterministic exports/HTML, causal prefixes, metadata and invalid/failed-run accounting.
Its temporary study artifacts are removed after the check; it needs the local
`storage/databases/1d_cmc.csv` dataset. It does not replace Step59.

`python3 validation/research_scenario_gate.py` checks all eight CURRENT scenario
strategies on bounded daily crypto, stock and 4-hour inputs. It reconciles accounts
and long/short PnL, fees, duplicate campaigns, deterministic HTML/exports and exact
shorter-history prefixes. It checks daily HTML-runner baseline parity, bounded
buy-and-hold curves, dataset benchmark/SMA/annualization settings, warmed configurable
benchmark fixtures, default selections and error counters. Actual report JavaScript
Pearson calculations are compared with independent daily-return calculations using
Node (WSL Node or the installed Windows Node via WSL interop); no browser is launched.
The required daily/stock/4h CSVs remain local under ignored `storage/databases/`.

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
