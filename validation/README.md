# Validation guide

`python3 validation/paper_comparison_test.py` exercises the explicit CURRENT fast
PAPER baseline: indicator-only warmup, next-open quantity/fills, commissions,
deterministic rerun, poisoned future-close exclusion, signed exposure and date joins.
Go provider tests additionally cover missing/stale comparison artifacts and strict
50/40 diagnostic boundaries. The full Step59 gate remains required before the
cross-cutting PAPER deployment; the historical fingerprint must remain unchanged.

Mobile dashboard access was cancelled by the user on 2026-10-09. Its retained tests
below are optional historical preparation; handset acceptance is no longer pending.
The [shadow-trading local rehearsal](../docs/ROADMAP.md#shadow-trading-preparation-and-acceptance)
is implemented and passed with a no-submit boundary; selected-venue forward
observation and its prerequisites remain pending.

## Local shadow rehearsal

From the repository root under WSL, with local Docker running:

```bash
python3 validation/shadow_trading_test.py
```

The command uses the current accepted local image tags by default:
`algotrading-runtime:risk-evaluations`, `algotrading-telegram-api` and
`algotrading-browser-web`. It does not build or fetch an updated trading image;
build current source first when these packages change. `--runtime-image`,
`--api-image`, `--web-image` or PAPER_*_IMAGE variables select other validated tags.
Cached infrastructure images are NATS 2.10, PostgreSQL 16, Alpine 3.20 and Go 1.23.
An existing byte-identical compiled market fixture is reused when available.
Python requires PyYAML, as in the existing PAPER study.

The isolated PAPER study produces 200 completed BTC/ETH bars, two USD 10000 buy
intents at close(T)=199, simulated next-open fills at 200 and unchanged economics
after worker restart. `PAPER_SHADOW_TEST=1` hands its actual saved plans and source
configuration fingerprint to the separate observer. Generated error cases are
explicit mutations of that input, not new strategy outputs or a backtest comparison.

Checks cover duplicate/restart observations, conflicting identity/configuration,
unavailable perpetuals, minimum quantity, quiet/cancel-intent cycles, stale/future/
incomplete data, HTTP error/disconnect/redirect recovery and a spy showing only
GET /snapshot with zero private writes. Producer plan bytes remain unchanged.
The reader accepts loopback only; the PAPER service network is internal and fixtures
force TEST_FILE notifications with Telegram credentials cleared.

Reports/input/SQLite/call evidence stay under ignored
`storage/paper_validation/<random-project>/shadow/`; the command prints the HTML path.
Only its own random project containers/networks are removed, preserving volumes.
No normal PAPER/VPS state, private orders or real Telegram sends are changed.

For quick observer regressions without Docker:

```bash
python3 validation/shadow_trading_test.py --unit
```

Actual venue coverage, symbol/currency mapping, mark-price freshness, full venue rules,
account/collateral/funding, private lifecycle, forward daily acceptance and comparison
with the aligned simplest backtest remain pending. A fixture report is not proof of
profitable trading, order execution or selected-venue readiness.

## Other component checks

Kraken public Multi-M preparation is tested under WSL:

```bash
python3 validation/kraken_shadow_test.py
python3 validation/shadow_trading_test.py --unit
python3 validation/kraken_shadow_test.py --snapshot storage/kraken_shadow/local-20261010-btc-usd
```

Thirteen focused regressions cover PF_ identity, precision/scaled assets, empty-book
retry, inactive/inverse exclusion, clocks, duplicate metadata, raw public GET bounds,
journal restart/conflicts/source isolation and hypothetical BTC/USD index/haircut,
USD reserve, PnL/funding signs and margin shortfalls. The offline audit checks raw
hashes, fixed GET routes, normalized catalogue, source-plan identity, preserved
observations and scenario arithmetic. These tests use no credentials or private
routes. The public capture is opt-in through `live_trading/kraken_shadow.py`; see the
[guide](../live_trading/README.md#kraken-multi-m-preparation). The legacy demo probe
redirected and remains unaccepted; confirm the supported test environment before
private integration. Hypothetical checks do not establish actual account admission.

Kraken account preparation is tested offline under WSL:

```bash
python3 validation/kraken_account_test.py --study storage/kraken_shadow/account-local
```

Twenty-six tests cover signed fixed GETs and permission gating with synthetic in-memory
credentials, venue margin/PnL/funding without double counting, BTC/USD reserves,
short positions, partial-order observations, schema/clock errors, sanitized connection
errors, redirects/response bounds and journal duplicate/conflict/restart/source/policy
isolation. The optional study writes synthetic fixture/policy/report JSON, HTML and
SQLite under ignored storage/kraken_shadow; no network or credentials are required.
Evidence: storage/kraken_shadow/account-local-20261010/report.html. Actual key loading,
private account acceptance and trading lifecycle tests remain pending for that fixture.
The account CLI additionally has offline loader/CLI tests with synthetic temporary
credential files: quoted/plain UTF-8 BOM/CRLF, missing/duplicate/malformed values,
secret-free corrective errors, HTTP 401 versus 403, explicit User-Agent and safe
failed-attempt status without accepted reports. Tests never read the user's .env.
Real opt-in `python3 live_trading/kraken_account.py --account` passed read-only
inspection; normalized evidence is under ignored storage/kraken_shadow/account-live/.
No orders, transfers or actual policy are implied by this acceptance.

Relative-policy checks cover the user's USD 50 floor and 10% available USD using
net venue marginEquity, never leveraged notional. Tests include independent exact
boundaries, zero/negative equity, missing USD, invalid fractions/fields, loss stress
separate from current cash, policy-bound journal isolation and CLI export. Actual
policy evaluation uses --policy storage/kraken_shadow/account-policy.json and a
separate --output storage/kraken_shadow/account-live-policy-50-10; its ignored report
retains low USD-reserve warnings. No automatic conversion or exchange stop is tested
or activated; remaining venue/accounting/recovery acceptance and future cost forecasts are pending.

Kraken account warning/recovery projection is checked under WSL:

```bash
python3 validation/kraken_account_monitor_test.py
KRAKEN_MONITOR_DOCKER_TEST=1 python3 validation/kraken_account_monitor_test.py
```

Eight offline tests cover grouped reserve warnings, critical margin/floor alerts,
restart deduplication, source/policy rejection, failed/stale reads without false
recovery and history republishing. The optional ninth test uses the existing local
Go notifier image with TEST_FILE, no network and no credential file: warning and
resolution delivery survive notifier restarts. It never sends Telegram messages.
User-authorized real low-reserve delivery was accepted separately; sanitized source
status and receipt-count evidence are in storage/kraken_shadow/account-alerts/.

`python3 validation/kraken_portfolio_summary_test.py` adds five offline tests for
UTC scheduling, once-per-day/restart deduplication, USD balance/equity distinction,
marked LONG/SHORT exposure, previous-day last-snapshot selection and absent/stale
account/price evidence. Go notifier tests cover compact severity/account/system text,
identity omission without breaking durable receipts and calendar summaries across
the operational warning cooldown. The account notifier admits INFO explicitly.
`python3 validation/telegram_message_preview.py` writes ignored synthetic examples;
--send is an explicit real Telegram operation, sends three TEST-labelled messages
and retains safe receipts under storage/telegram_previews/. No credential file is
opened by assistant tools or included in test output.

The monitor suite now has thirteen tests including the optional Go contract case.
Account warning checks cover an exact 10% daily-equity boundary without stop/order
mutation, daily comparison rather than peak drawdown, missing-day retention without
false resolution, exact 5x acceptance versus greater-than-5x warning and unavailable
public marks retaining the previous exposure warning. status.json exposes unavailable
checks separately from authenticated source health. The user cancelled additional
new-order admission/limit work. Portfolio tests additionally verify USD exposure /
the snapshot's own equity percentages, SHORT values, percentages above 100%, and
nonpositive equity remaining unavailable. No order or sizing changes are included.

Public exchange coverage uses `python3 tools/exchange_coverage.py` (network required,
public endpoints only, no authentication). It ranks actual Binance crypto USDT
perpetual quote turnover over 25 completed UTC days and writes ignored timestamped
raw/JSON/CSV/HTML evidence under `storage/exchange_coverage/`. Current listings are
a survivorship-biased reference; public matches do not establish account eligibility.

Run under WSL:

```bash
python3 validation/exchange_coverage_test.py --snapshot storage/exchange_coverage/20261010T110714Z
```

 Five focused regressions
and an offline audit rederive every saved candle total, complete ranking, active
venue filters, explicit aliases and per-asset matches/counts. No API calls or trading
state changes occur during this audit. Replace the snapshot path for later captures.

Run `python3 validation/mobile_access_test.py` under WSL with OpenSSL installed for
the temporary phone bridge. Six bounded tests check TLS certificate verification
and reuse, wrong-IP rejection, HTTP authentication status/cookie/body forwarding,
SSE arrival before upstream closure, peer-IP exclusion, upstream loss and refusal
of public/wildcard addresses. HTTP authentication here uses an isolated fixture;
actual phone login/page/logout acceptance is separate. No VPS or private services
are contacted.
An optional seventh test uses `MOBILE_TEST_API_PORT` and `MOBILE_TEST_API_PASSWORD`
for an isolated localhost Go API fixture: real login, authenticated page reads,
viewer restrictions, CSRF enforcement, logout and session revocation through TLS.
Use disposable fixture credentials and embedded mock data; never point this logout
test at the normal dashboard or VPS. The test does not create its API fixture.

Local Telegram preparation is checked by `go test ./...` in `dashboard/dashboard-api`
under WSL (the cached Go Docker image also works without external network access).
Notifier tests use an HTTP fixture, not Telegram: successful/rejected delivery,
sanitized errors, bounded timeout, retry_after deferral across alerts, retry without
consuming the event, and durable receipt recovery after a lost decision checkpoint.
Provider tests cover fresh process loss, NTP unsynchronization, host pressure and
daily progress, plus unavailable evidence and recovery. The packaged notifier's
explicit `--test-message` command rejects TEST_FILE mode. The
[setup runbook](../dashboard/README.md#local-telegram-setup) describes opt-in PAPER
configuration and the real one-message test, which requires the user's destination.
Do not describe simulated tests as Telegram delivery acceptance.
`telegram_continuous_test.py --send-real` explicitly authorizes two real TEST ONLY
messages from an isolated watchdog-format journal. It requires a local Docker
endpoint, the current `algotrading-telegram-api` image and ignored local credentials.
It checks failed transport without event consumption, restored delivery, stale-source
blocking/recovery and fresh sweeps after two restarts with exactly two receipts.
Only its own container/state volume are removed; ignored evidence is retained.
This is notifier transport/lifecycle evidence, not actual trading-fault evidence.

Pipeline/Risk provider regressions are in the Go suite: quiet/HOLD cycles, missing
signal/target versus explicit FLAT, absent decision/planner versus database failure,
persisted policy settings, multi-strategy capital weighting and replay observations.
Risk evaluation regressions also check signed sizing, HOLD interpretation, missing
numeric values and cycle/configuration mismatch rejection. The SQL/HTTP fixture
starts with an older schema, adds the nullable report and verifies both paths.
For packaged real-provider acceptance, build and run from the repository root in WSL:

```bash
docker build -t algotrading-pipeline-risk-api -f dashboard/dashboard-api/Dockerfile dashboard/dashboard-api
python3 validation/pipeline_risk_read_model_test.py
```

The fixture requires local Docker and cached `postgres:16-alpine`, refuses remote
Docker endpoints, creates its own PostgreSQL/API containers and cleans only those
containers/anonymous volumes. It verifies authenticated HTTP/SQL reads, quiet/HOLD
policy evidence, missing planner, timestamp mismatch rejection, first-decision
pending and unchanged account cash. Credentials/evidence remain under ignored
`storage/pipeline_risk_validation/`. This is a read-model fixture, not a trading
service or private-exchange acceptance campaign.

`portfolio_risk_diagnostics_test.cpp` checks exact captured/default DecisionBatch
equivalence, same-date capital, signed sizing, asset/gross reductions, HOLD/FLAT,
volatility transformations, missing covariance and optional diagnostic failure.
Compile it under WSL with C++23 and the `lib/src/*` include directories, linking
`lib/src/risk/portfolio_risk_engine.cpp`. Meaningful risk changes also require Step59.
`paper_trading_test.py` checks new persisted Risk evaluations against the actual
packaged services and verifies identical reports after restart, alongside fills and
accounting. Optional `PAPER_RUNTIME_IMAGE` and `PAPER_API_IMAGE` select fresh local
images without retagging another stack; the defaults require rebuilding current
sources. Use only local Docker. Its isolated test state remains under ignored storage.

For bounded authenticated browser acceptance, build the current frontend under WSL
with `VITE_DASHBOARD_DATA_MODE=api` and `VITE_DASHBOARD_API_BASE_URL=/api`.
Select current packaged images using `PAPER_RUNTIME_IMAGE`, `PAPER_API_IMAGE` and
`PAPER_WEB_IMAGE`, then run `PAPER_BROWSER_HOLD_SECONDS=300 python3 -u
validation/paper_trading_test.py`. After its service/accounting/restart assertions,
it prints `Browser fixture ready` and creates `browser-ready.json` in its unique
ignored directory. From Windows, during that bounded hold:

```powershell
python validation/dashboard_browser_test.py --fixture-directory storage/paper_validation/<printed-project>
```

The test requires Edge (or `--browser`), `requests` and `websocket-client`. It reads
only the isolated fixture credentials, exercises invalid/valid login, all eleven
authenticated pages, Pipeline expansion/inspector/search, persisted Risk evidence,
stale Infrastructure telemetry, viewer mutation rejection, resource error/retry
and logout protection. It retains screenshots and `browser/accepted.json` without
credentials. Removing the readiness marker releases the fixture; its services stop
after at most 300 seconds even if no browser runs. The fixture forces TEST_FILE
notifications and clears Telegram credentials. It leaves other projects unchanged
and retains its own stopped containers/volumes as evidence. This does not accept
operator routing, host/backend connectivity failures, mobile access or VPS behavior.

Overview chart checks are frontend-only. Build under WSL, then run the focused
date-window tests with the build image's esbuild/Node:

```bash
docker build --target build -t algotrading-overview-build -f dashboard/Dockerfile dashboard
docker run --rm --network none -v "$PWD:/repo:ro" --entrypoint sh algotrading-overview-build -c 'NODE_PATH=/app/node_modules /app/node_modules/.bin/esbuild /repo/validation/overview_chart_test.ts --bundle --platform=node --format=cjs --outfile=/tmp/chart-test.cjs && node /tmp/chart-test.cjs'
```

`python validation/overview_chart_browser_test.py --url http://127.0.0.1:8094`
checks actual interactions against an isolated **frontend-local mock** preview.
It requires Python `requests`, `websocket-client` and a Chromium browser
(`--browser` overrides the Windows Edge default). All six timeframe buttons,
hover, independent X/Y zoom, two-axis pointer pan and reset are checked; the
screenshot/profile goes under ignored `storage/overview_browser/`. The WSL build
and desktop browser check are separate. The script does not start a preview,
authenticate a real account or accept VPS/trading behavior.

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

`PAPER_RECOVERY_TEST=1 python3 validation/paper_trading_test.py` adds the local
recovery study in `paper_recovery_test.py`. Use current packaged runtime/API/web
images (the same optional image overrides as the normal fixture). This opt-in is
separate from browser hold. It stops all four dashboard services before actual
ingestion/fills, cuts HTTP and NATS connectivity, and checks completed-data/fill
boundaries. NATS loss may retain already committed completed SQLite bars; retry
must deliver the cycle without duplicate economics. PostgreSQL outage must produce
an explicit HTTP 503 and retain economic state after controlled worker restart.

The study quiesces all writers/broker, makes a PostgreSQL custom logical dump and
archives the stopped market/NATS/watchdog/audit/acknowledgement/notifier volumes.
Checksums are verified before restoring into another unique project with fresh
volumes. It compares every public SQL table and archived file before workers run,
then checks same-day ingestion, cash/positions, order/fill IDs and authenticated
Risk data after restart. Credentials remain separate and ignored; backups and
`recovery-accepted.json` stay in the fixture directory. Original/restored volumes
are retained; only owned fixture containers/networks are removed after restoration.
This is a maintenance-stop rehearsal with the same images/settings, not an online
multi-store backup, automatic reconnect proof, off-host retention policy or VPS
disaster-recovery acceptance. No Telegram delivery or private orders are performed.

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

`python3 validation/host_monitor_test.py` checks verified/unsynchronized/unavailable
NTP observations and process probes without exposing command arguments. Under WSL,
`HOST_MONITOR_DOCKER_TEST=1 python3 validation/host_monitor_test.py` additionally
uses an isolated local container to distinguish a running, stopped and absent
executable from container liveness, and prints the actual host NTP observation.
It requires the locally available `golang:1.23` image and cleans only its own container.
Provider tests cover daily UTC progress/grace, no-order cycles, stalled dates,
missing first snapshots, stale telemetry and clock evidence without granting trading.
The packaged `pipeline_risk_read_model_test.py` PostgreSQL/HTTP fixture also verifies
Infrastructure process/clock observations and missing/stalled evidence; set
`DASHBOARD_TEST_API_IMAGE` to the freshly built API image when using a custom tag.

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
