# Current state

Last synchronized: 2026-10-16. Current source and test output take precedence.

## Position fee colors (2026-10-19, local only)

Open/closed position fee cells and Total Fees now use the existing sign-based
color helper: negative red, positive green, zero and unavailable neutral.
Amounts and fee accounting are unchanged. Frontend TypeScript and production
Vite build PASS. No backtesting or VPS deployment.

## ALERT severity for unavailable Kraken signals (2026-10-19, local only)

Added INFO < WARN < ALERT < CRITICAL throughout dashboard alert types, priority
sorting, filters, independent ALERT counts, durable watchdog heartbeats and notifier
severity admission. An active strategy signal on an asset Kraken cannot trade is
now ALERT; unavailable assets without active signals remain WARN/INFO as before.
Wrong symbol conversion scales remain CRITICAL. Mapping admission and automatic
trading behavior are unchanged; alarm severity does not enable unsupported orders.

Telegram ALERT notifications and active-summary entries use an orange circle and
the general chat, including resolutions (green/silent). Only CRITICAL and their
resolutions use the optional critical group. ALERT retains normal notification
sound and does not use the CRITICAL-only delay. Existing receipts/history remain
intact; previously delivered critical messages are not rewritten.

Debian Linux cloud validation: go vet and all Go package tests PASS, including loopback-only
Telegram payload/chat routing tests, severity filtering/escalation, normal-chat
summaries, durable ALERT persistence/counts and symbol-map severity/counts.
Frontend TypeScript + production Vite build PASS. No backtesting, real Telegram
messages, exchange orders or VPS deployment were performed.

## Infrastructure ALL range (2026-10-18, local only)

Added ALL to the infrastructure time selector and range=all to the host-history API.
ALL discovers the earliest valid retained observation, including unsorted rows, and
uses that span for chart bounds and averaging. File size remains capped at 64 MiB;
responses remain bounded to 720 averaged points. Current monitor retention remains
seven days: ALL means all available retained data, not deleted observations.
No VPS changes. WSL validation PASS: offline Go vet/tests for all packages, frontend
API-mode Docker build (tsc -b + Vite), source audit (33 markers), targeted diff check.

## Symbol Mapping problem table (2026-10-18, local only)

Removed the Detail column from Problems Found; rows show Severity, Asset and Problem.
Alert payloads and the main mapping table are unchanged. No VPS deployment or exchange
commands. WSL frontend Docker build (API mode, tsc -b + Vite) PASS.

## Market freshness age in days (2026-10-18, local only)

Freshness ageLabel now displays Nd relative to the previous completed UTC day,
not relative to the newest date in SQLite. A candle dated Oct 17 stays 0d through
Oct 18 23:59:59 UTC and becomes 1d on Oct 19. This keeps age advancing if ingestion
stalls. Existing health classification against the canonical day is unchanged.
Only the dashboard read model changes; no exchange commands or VPS deployment.
UTC boundary/time-zone regression test added. WSL validation PASS: go vet ./...
and go test ./... in offline golang:1.23-alpine; targeted git diff --check PASS.

## Execution fees and reference slippage (2026-10-18, local only)

User requested local fixes only; no VPS deployment or real orders authorized for this task.
Root causes: dashboard fill readers ignored delayed Kraken FEE events in
trading_venue_costs, the tracked-order DTO omitted persisted reference_price, and
generic frontend currency formatting rounded small fees/prices to cents. The local
read projection now aggregates linked fees once by fill_id, adds them to fill commission
as ExecutionState accounting does, and derives side-aware reference slippage from
persisted prices (stop trigger for position stops). Historical missing reference prices
remain unavailable; no mark-at-submission history is invented. No lib/, execution
service or gateway behavior was changed by this task.

Read-only VPS SELECT confirmed fills 1/2/3 have commission=0 while linked FEE cash
events are -0.0068 / -0.0069 / -0.0074 USD: recorded fees total 0.0211 USD. Their
planning references are already persisted (NEAR 5.067, STRK 0.0741, WLD 0.5167).
Local projection joins those events for per-fill/order/global fees and the fill-ledger
read. Small fees retain eight decimals; Kraken unreported fees stay Pending rather
than becoming a claimed zero. PAPER zero fees remain zero. No source rows are rewritten.
Reference slippage is side-aware, daily and USD-notional weighted; per-order values
use VWAP, stop fills use the trigger, missing old references remain unavailable and
truncated fill windows suppress incomplete quality summaries. It includes gaps and
venue price differences and must not be read as isolated market impact at submission.

WSL validation: all Go packages `go vet ./...` and `go test ./...` PASS offline in
golang:1.23-alpine; frontend Docker build (API mode, tsc -b + Vite) PASS;
dashboard source audit PASS (33 markers); targeted git diff --check PASS.
`python3 dashboard/scripts/test-execution-fees-postgres.py` PASS with isolated,
network-disabled PostgreSQL: legacy schema, delayed/multiple/zero fees, pending
fees, no duplicated fills, exclusion of funding/interest/orphan fees, global totals,
bounded detail and repeatable reads. Canonical Step59 PASS: 107 days, 50 fills,
RealTest 25/25 with zero differences, deterministic rerun, system/dashboard restart
and pacing invariance. Accepted fingerprint:
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`.
Read-only evidence: `storage/reports/execution_quality_20261018/vps_readonly_evidence.json`.
Concurrent pipeline/previous-signal work in the shared reader was preserved.

## Active-alert summary rows: color and name only (2026-10-18, deployed)

User correction after deployment: summary rows must omit bracketed severity names.
Shared Watchdog/Kraken summary formatting now emits `🔴 gateway-kraken down`,
`🟡 USD reserve low`, etc. Severity still selects the color and destination; individual
alert headers, portfolio reports, summary headings and delivery policy are unchanged.
Go vet and tests for internal/notifier and cmd/dashboard-alert-notifier PASS in WSL
using offline golang:1.23-alpine. Existing summary tests updated and regression assertion
rejects bracketed labels. README/chain guide updated. User explicitly authorized deployment
at 12:44 UTC. Release `20261018_130842-9bed096-dirty` activated at 13:13 UTC; previous
`20261018_121047-9bed096-dirty`. Full `algo build` PASS, including web tsc/Vite.
Concurrent provider valuation edits were detected during packaging: local package
`20261018_130038-9bed096-dirty` is marked NOT_FOR_DEPLOYMENT and was never uploaded.
The final package was created from the previous release's source plus only the summary
formatter/tests and documentation. The API image was rebuilt from that isolated source;
all Go tests and `go vet ./...` PASS offline. Source comparison confirms the provider
valuation code is unchanged from the previous deployed release.
Local/remote SHA256SUMS PASS; manifest IDs match the completed builds and running images:
runtime `7a16e58b...`, API/notifiers `1d30c2a5...`, web `71daac66...`.
Activation backup: `/algoTrading/backups/20261018_131316.tar.gz`.
All 13 containers healthy; API health OK; Kraken host monitor active with fresh successful
observations. Execution startup reconciliation clean, 3 positions / 3 fills / stops 7/8/9
preserved. No manual real orders or test Telegram messages were sent. Daily portfolio
tables are unchanged. Evidence: `storage/reports/telegram_summary_color_only_20261018/`.

## Kraken daily portfolio tables and Telegram deployment preparation (2026-10-18)

The user authorized applying the pending changes to the VPS, then requested readable
tables in the daily Kraken portfolio. `kraken_portfolio_summary.position_table` now
renders Asset / Side / USD / % Eq for current positions and the previous UTC day's last
snapshot. Persisted position lines keep their original format, so existing history works
without migration. Missing prices/equity remain N/A; missing yesterday stays unavailable.
The notifier uses escaped HTML `<pre>` only for DAILY_PORTFOLIO to preserve columns;
ordinary alerts remain plain title-only messages. No new report is forced or replayed.

Validation (WSL): kraken_portfolio_summary 8 tests PASS; kraken_account_monitor 20 tests
OK (one existing environment-dependent skip); all Go tests and `go vet ./...` PASS in
offline golang:1.23-alpine; dashboard source audit 33 markers PASS; installation-root
test PASS. No manual real orders or Telegram test messages.

VPS read-only preflight 11:54 UTC: current `20261018_114445-9bed096-dirty`, all 13 containers
healthy, Kraken monitor active, no other build/activation detected. Comparing its archived
application source with local source found only the notifier formatting/tests and portfolio
summary changed. Deployment coordination: `docs/codex/DEPLOYMENT_IN_PROGRESS.md`.
Deployment completed at 12:14 UTC: release `20261018_121047-9bed096-dirty`, previous
`20261018_114445-9bed096-dirty`. `algo build` PASS (82 targets, runtime/API/web images;
web tsc/Vite PASS). Manifest IDs equal the completed builds and the running containers:
runtime `77601d5a...`, dash-api/notifiers `ded666dd...`, dash-web `f65db1a2...`.
Local and remote SHA256SUMS passed; activation took backup
`/algoTrading/backups/20261018_121352.tar.gz`. All 13 containers healthy, API health OK.
Kraken read-only host monitor restarted to load the new table code; its installed SHA-256
matches local source and lastSuccessAt advances without an error. Retained daily snapshots
for Oct 18 and Oct 17 (last observation 23:59:53 UTC) render as tables on the VPS.
Execution startup reconciliation clean; 3 positions, 3 fills and accepted stops 7/8/9
preserved, no duplicate orders. Snapshot cash at verification: 41.9493 USD (venue-aligned).
No report was forced/replayed; the next normal 09:00 UTC report uses the new presentation.

## Telegram: CRITICAL labels, title-only alerts and colored summary severity (2026-10-18, local)

User request: replace Telegram's URGENT wording with CRITICAL, omit alert descriptions,
and show a colored severity beside every name in active-alert summaries. The notifier now
retains the account/system header and alert title only, including recoveries and source-health
warnings. DAILY_PORTFOLIO is a report and retains its balances/holdings. Descriptions remain
in the web and journals. Summaries use red `[CRITICAL]`, yellow `[WARN]`, blue `[INFO]` and
white for unsupported labels; the critical-chat heading is "Critical alerts". Recovery
messages retain green INFO and silence. Delivery routing, receipts, cooldown and critical
delay are unchanged. README updated.

Validation (WSL, local `golang:1.23-alpine`, `--network none`): `go vet ./...` and
`go test ./...` PASS, including title-only messages, retained portfolio content, colored
summary rows, per-chat routing, critical-delay suppression and resolution. No VPS access,
deployment or real Telegram messages. Pending an explicitly authorized deployment.

## Overview: compact Collateral card and Exposure card (2026-10-18, local)

User request: Collateral shows only the total USD collateral (Kraken USD balance + BTC after
haircut) and the BTC share, e.g. "$226.83 · 55% BTC" (no BTC quantity); a new "Exposure" card
"Gross $x (y% of Total Equity) · Net $z" from the positions valuation (all modes with a
valuation). Go tests OK (updated TestKrakenCollateralCardsOnlyInLive, new
TestOverviewShowsGrossAndNetExposure). Release 20261017_235036-9bed096-dirty built 23:50 UTC
(dash-api 13b824ff...). The user asked to publish inside the 23:30-00:30 window, which
`algo activate` refuses by design; with the user's explicit instruction only dash-api (no
trading role) was swapped at 23:51 UTC: release uploaded (SHA256SUMS OK), images.tar loaded,
`algotrading-dash-api:chain` retagged to 13b824ff..., `algo up --no-deps dash-api`; dash-api
healthy, every trading container untouched. releases/current still names 20261017_190518:
the formal `algo activate` of 20261017_235036 is due after 00:30 UTC.

Follow-up (2026-10-18 00:10 UTC): the Exposure card was split into two cards, "Gross Exposure"
(detail: % of Total Equity) and "Net Exposure", as on Positions. Release
20261018_000946-9bed096-dirty (dash-api 9baf54e7...) was hot-swapped the same way (dash-api
only, healthy; execution/gateway-kraken/dash-web still on b6e6e423.../a15b1312...). This
release supersedes 20261017_235036 for the pending formal `algo activate` after 00:30 UTC.

First daily stop move verified (00:00 UTC cycle, read-only): new stops 7/8/9 at 70% of the
2026-10-17 closes (NEAR 3.402, STRK 0.04515, WLD 0.3445 at venue tick) accepted first, then
old stops 4/5/6 cancelled. Kraken shows exactly 3 open orders: stop, reduceOnly, trigger mark,
cliOrdId algo-kraken3-7/8/9, matching the 3 long positions.

Formal activation (2026-10-18 09:58 UTC, user-authorized): `algo activate
20261018_000946-9bed096-dirty`; releases/current names it, previous 20261017_190518 (rollback).
All 13 containers healthy; runtime 622f4bef... (same source, rebuilt), dash-api 9baf54e7...,
dash-web 495caff5.... Execution restart reconciled clean (cash 41.96 USD, 3 positions, 3 open
orders); Kraken still shows only stops 7/8/9, no duplicates.

Overview cards (2026-10-18 10:21 UTC, local, not deployed): the Margin Headroom card was removed
and `orderOverviewStats` sorts the cards Total Equity, Gross Exposure, Net Exposure, Collateral
(Cash outside Kraken LIVE), Leverage, then any other card (Net Deposits). Go vet/tests OK
(TestOverviewShowsGrossAndNetExposure checks the order); API_CONTRACT.md updated. Release
20261018_102127-9bed096-dirty built (dash-api a4e62aa2...) and formally activated 10:23 UTC
(user-authorized; previous 20261018_000946 for rollback). 13 containers healthy; runtime
25a3c31e..., dash-web 23094a8e...; execution reconciled clean (cash 41.95 USD, 3 positions,
3 open orders).

Market-data same-day restart (2026-10-18, deployed 11:45 UTC as release
20261018_114445-9bed096-dirty, user-authorized; previous 20261018_102127 for rollback; all 13
containers healthy, runtime 80aa7fca...; the restart logged "already stored ... republishing",
0 rows, no ranking; execution reconciled clean, cash 41.95 USD, 3 positions, 3 open orders.
The user chose to keep the 54 tracked symbols.): `MarketDataIngestor::run`
now returns `republishStoredDay` when the stored tracker date equals the target day: summary
from SQLite (stored ranking count, tracked/maintained), no exchangeInfo/24h ticker/candle
requests; only open(T+1) prices are fetched when `publish_paper_execution_prices` is set, for
the symbols with a stored candle that day. paper_trading_test asserts the restart run logs
"is already stored" and no "Download plan". Validation: ninja build OK, chain build OK,
PAPER_RECOVERY_TEST=1 paper_trading_test PASS (incl. NATS-outage retry and restore),
daily_service_logs_test OK, Step59 PASS fingerprint 94fdf8d8...68da2. The VPS market database
still holds the 09:58 re-rank of 2026-10-17 (ranking overwritten, 54 tracked); untouched.

Market-data midnight run, second step (2026-10-18, deployed 18:56 UTC in release
20261018_185456-9bed096-dirty, user-authorized; previous 20261018_171838 for rollback):
10 s before midnight `MarketDataIngestor::prepare` reads the tracker, builds the expected plan
and opens the HTTP/2 connection (`warmUp`; the client keeps one curl multi handle, so the
connection survives between calls); the business wait sleeps exactly to the deadline (was a
250 ms poll); `midnight_delay_ms` 0 (VPS config edited 100 → 0, backup
market_data.json.bak-20261018b). The run is one burst (`fetchDailySnapshot`): exchangeInfo,
24h tickers and every planned symbol's candles up to D plus D+1. The D+1 candle proves D closed
on Binance's clock and gives open(T+1): the /fapi/v1/time request and the separate opening-price
requests are gone from the normal path. Symbols without D+1 are re-asked every 50 ms for up
to 30 s, then Binance's clock decides; the ranking needs tickers' closeTime >= midnight
(fallback: Binance's clock). Local two-day run against real Binance: 614 ms prepared (5.3 s
before today's work); D+1 never stored. Validation: ninja OK, time-handler audit PASS, chain
build OK, PAPER_RECOVERY_TEST=1 paper_trading_test PASS, daily_service_logs_test OK (alone),
Step59 PASS 94fdf8d8...68da2. After activation: 13 containers healthy, execution reconciled
clean, market-data republished the stored day.

Market-data daily download speed-up (2026-10-18, deployed 17:20 UTC in release
20261018_171838-9bed096-dirty, user-authorized; previous 20261018_144907 for rollback). The
00:00 run took 7.2 s: clock 0.26 s, then exchangeInfo (1.1 MB, uncompressed) + 24h tickers,
then 50 candles and 50 opens on 16 connections. Measured on the VPS: each Binance request
answers in ~0.25 s; 100 klines take 0.8 s multiplexed on one HTTP/2 connection vs 2.5 s on 16
connections and 3.8 s on one each; gzip shrinks exchangeInfo to 52 KB and tickers to 65 KB.
Changes: `BinanceMarketDataClient::getAll` (curl multi, CURLPIPE_MULTIPLEX + PIPEWAIT,
retries as httpGet) used by the ranking, candles and opening prices; CURLOPT_ACCEPT_ENCODING;
after the Binance clock check the ranking, the previous tracker's candles and the previous
day's symbols' opens download together, only symbols the ranking adds wait for it;
max_parallel_requests 64 (schema maximum; VPS /algoTrading/config/market_data.json edited
16 → 64, backup market_data.json.bak-20261018); new INFO line "Daily download for D took N
ms: Binance clock, ranking, candles (early/after/retried), opening prices, saved". Weight ~150
of Binance's 2400/min. Local two-day run against real Binance: 5.3 s → 1.2 s. Validation:
ninja OK, chain build OK (runtime libcurl links nghttp2 and zlib), PAPER_RECOVERY_TEST=1
paper_trading_test PASS, daily_service_logs_test OK, Step59 PASS 94fdf8d8...68da2. Same
release: Alerts & Audit rows hide the long description behind a "Details" button. After
activation: 13 containers healthy, execution reconciled clean (41.93 USD, 3 positions, 3
orders), market-data republished the stored day. Check tonight's "took" line.

Release 20261018_144907-9bed096-dirty activated 14:50 UTC (user-authorized; previous
20261018_130842, activated 13:13 by another session, for rollback). It carries the Pipeline
Trace, the HOLD target on Positions and the lower-case Kraken funding contract fix. 13
containers healthy; runtime d4bfc102..., dash-api ede3b1e7..., dash-web 98fec553...;
execution reconciled clean (cash 41.94 USD, 3 positions, 3 open orders); market-data restart
republished the stored day (no download); dash-api has
DASHBOARD_STRATEGY_CONFIG=/config/strategies/pure_rsi_8_50_40_volume_rank.json mounted. The 38
stored funding rows with lower-case coin are still uncorrected (needs authorization).

Pipeline Trace (2026-10-18, deployed 14:50 UTC in 20261018_144907): the Decision Matrix "Why?" button is now
"Trace" and its panel explains the whole cycle with real numbers (`real_pipeline_trace.go`,
`enrichPipeline`): liquidity rank (#n of candidates by the strategy's SMA measure, inside/outside
the top-N universe) and RSI recomputed from the market SQLite; the PureRSI rule with the live
parameters; signal transition against the previous risk checkpoint (`previousSignalsPayload`
in pipelineCheckpointSQL); sizer by configuration (equal weight: signal × weight per signal;
volatility target: book volatility, scale, after caps; "Vol targeting" shown only then); asset
and gross caps; rebalance band arithmetic (relative 25%: current 8.6% = 4 × 4.86 / 226.57 vs
target 10%, 1.42 pp < 2.15 pp → HOLD); fees and slippage of the cycle's fills; risk config
fingerprint and venue. HOLD rows show "Target" as approved weight × reference capital
("22.66$ (hold)") and "Liq. Rank" as "#n". Also found: the dashboard used RSI(7) and 80/70
(DASHBOARD_RSI_ENTRY/EXIT unset on the VPS) while the live strategy is RSI(8) 50/40; it now
reads the strategy file itself (`DASHBOARD_STRATEGY_CONFIG=/config/strategies/${STRATEGY_FILE}`,
strategies dir mounted read-only in dash-api and watchdog; `applyStrategyConfigFile`), which
also corrects the Market Data page. Go vet/tests OK (new TestTraceExplainsHoldInsideTheRelativeBand,
TestStrategyConfigFileOverridesEnvironmentDefaults), web tsc+vite build OK,
chain_installation_root_test OK, verify-project-contracts PASS.

Kraken funding not attached to positions (2026-10-18, fixed locally, not deployed): Kraken Multi-M
realizes funding hourly into the USD balance (account-log `realized_funding`), and the gateway
books it (38 rows in trading_funding by 13:00 UTC: STRK -0.025, WLD +0.009, NEAR -0.0009 USD,
cash included). But those rows name the contract in lower case ("pf_strkusd"), so
`SymbolMap::byVenueSymbol` missed and the payments were stored under the venue name instead of
STRKUSDT: per-position Total Funding stayed 0. `parseAccountLog` now upper-cases the contract;
venue_adapter_test PASS (302 checks; lower-case funding row reaches BTCUSDT, the 2026-10-15
real-shape row now expects PF_HYPEUSD). The 38 stored rows keep the lower-case venue name
until a data fix (not done; needs authorization).
Note: another session activated release 20261018_121047-9bed096-dirty at 12:13:56 UTC (not
this session); it does not contain addHeldTargets.

Positions target weight 0% on HOLD (2026-10-18, fixed locally, not deployed): with the 25%
relative rebalance band, risk kept NEAR/STRK/WLD on HOLD for 2026-10-17 and published an empty
decision (`strategies: []`, planner 0 orders: correct, positions kept). The dashboard read
target notionals only from the decision, so held assets showed target 0% and deviation -8.7%.
`addHeldTargets` now adds HOLD assets from the risk diagnostics as approved_weight x
reference_capital (10% of 226.57 USD). Go vet/tests OK (new TestHeldAssetsKeepTheirApprovedTarget).

Original report: restarting market-data during the day re-runs the
last completed day's ingestion with Binance's *current* 24h ticker ranking. The 09:58 restart
re-ranked 2026-10-17 with volume that includes 2026-10-18 hours, added 4 new top-50 symbols
(54 tracked) and downloaded their 100-day history (400 rows). Strategy/risk/planner/execution
had already processed 2026-10-17 and ignored the repeat (no new orders), but the tracked
universe now differs from a midnight-only run. Proposed fix: when the store's tracked date
already equals the target date, skip the re-rank/download and only republish stored data.

## VPS release 20261017_190518-9bed096-dirty: LIVE stop-loss 30% (2026-10-17, deployed)

Integration of tasks 0, A (Codex: gateway stp reduce-only mark-triggered orders, tick rounding
in decimal arithmetic, journal migration, fills by cliOrdId or journaled venue id, open stops
in snapshots; 300 adapter checks), B (Codex: simulated exchange resting stops, low <= stop,
min(open, stop), reduce-only cap, highest first; service persistence; README; Python service
test) and C (Codex: dash-api `stopLoss` per position, newest of two, STOP labels, WARN
"<asset>: no stop-loss" after 10 minutes in LIVE; API_CONTRACT.md) with task D. Claude added
`resting_poll_interval_ms` = 15 s to the gateway (fills/open orders polled every 15 s instead of
2 s when only accepted position stops are open) and adapted two adapter test polls plus a check
that a resting stop is not polled within 15 s. Pre-release (WSL): build; 20 component tests
(new position_stop_simulated_exchange_test registered); venue adapter 302 checks PASS; service
test POSITION-STOP-SERVICE PASS; chain_installation_root, host_monitor, paper_comparison,
standby, kraken_account_monitor, daily_service_logs OK; paper_trading_test PASS; 14 Go packages
OK; web and api images build; dashboard source audit PASS (33 markers); Step59 PASS fingerprint
94fdf8d8...8da2. README "Stop-loss (MODE=live)". Built 19:04 UTC (runtime b6e6e423..., dash-api
6a80b0aa..., dash-web a15b1312...; release.json equal), chain.env copy
backups/chain.env.before-stoploss-20261017, `LIVE_LONG_STOP_FRACTION=0.30` added, activated
19:06 UTC from 20261017_145336.
Incident found at activation: reconciliation reported Kraken with 0 positions and USD 102.15
against 3 local positions; trading paused. Kraken read-only fills show NEAR 4, STRK 305 and WLD
43 sold at 15:13 UTC without cliOrdId (outside the chain: manual close in Kraken), so no stop was
placed. User decision: restart now and re-buy. 23:14 UTC: comparison stopped, `algo down`,
data/live archived to backups/live-kraken2-20261017.tar.gz and removed,
LIVE_ACCOUNT_NAMESPACE=kraken3, LIVE_INITIAL_CASH=102.1531 (monitor usdBalance), `algo up`.
Reconciliation clean (102.15 USD, 0 positions, 0 orders); the 2026-10-16 decision replayed:
BUY 4 NEAR at 4.8543, 305 STRK at 0.06452, 43 WLD at 0.49 (taker); after each fill a stop:
NEAR 3.5469, STRK 0.05187, WLD 0.36169 requested -> 0.3616 at the venue tick (70% of the
2026-10-16 closes 5.067 / 0.0741 / 0.5167); all three accepted. Kraken read-only: 3 long
positions and 3 open orders type stop, reduceOnly true, triggerSignal mark, cliOrdId
algo-kraken3-4/5/6. 13 containers healthy. Pending: the first daily move at the 00:00 UTC
cycle (new stops at 70% of the 2026-10-17 closes, old ones cancelled after acceptance).

## LIVE long stop-loss: task D, execution lifecycle (2026-10-17, local)

`live_trading/execution_state_service/src/position_stops.{h,cpp}`: pure `planPositionStops`
(one stop per held long at (1 - fraction) x the latest completed daily close for the whole
position; a different price or size submits a new stop; older working stops are cancelled only
when the newest matching stop is Accepted/PartiallyFilled; flat positions cancel their stops;
a rejected stop is not resubmitted for the same price and size; held coins without a close are
reported and keep their stop). `ExecutionEngine::applyPositionStopChanges` stages submits and
cancels durably (persist) before sending, submits before cancels, accepts only position stops
and open, not yet cancelled stops. `makeExecutionPlanningStateSnapshot` hides position stops
from the order planner (no pending quantity, never a cancel candidate, revision unchanged by
replacements); `TrackedOrder::pendingSignedQuantity` is 0 for them. Execution service:
`--long-stop-fraction` (0 = off, [0, 1)); `maintainPositionStops` after a fill, an order
update, the applied daily plan ("daily close": every held long moves to the new close) and a
clean reconciliation (so existing positions get stops on the first start), never while the
daily plan is requested but not applied (`dailyPlanPending`, the plan's revision would no
longer match); cycle completion, the scheduled reconciliation skip and Manual Control ignore
open position stops; LG_ALERT once per rejected stop, LG_WARN once per coin and day without a
close. Chain: `LIVE_LONG_STOP_FRACTION` in chain.env (template 0), exported by ./algo in LIVE
as LONG_STOP_FRACTION, compose passes `--long-stop-fraction ${LONG_STOP_FRACTION:-0}`.
New `validation/position_stops_test.cpp` (placement, replacement order, following a lower
close and a new size, flat cancels, rejected not retried, missing close, engine staging and
planner exclusion). Checks (WSL): 19 component tests PASS, full build, `--help` lists the
option, chain_installation_root_test OK, paper_trading_test PASS, Step59 PASS fingerprint
94fdf8d8...8da2. Waiting for Codex tasks A (gateway), B (simulated exchange), C (dashboard);
the task 0 guards still reject position stops in the gateway and the simulator.

## LIVE long stop-loss 30%: design and task 0 contract (2026-10-17, local, in progress)

User decisions: a real Kraken stop-loss for every LIVE long at 70% of the latest completed
daily close, moved at every daily close (up or down, not a ratchet), reduce-only stop-market
with mark trigger, following the position size, cancelled when flat; re-entry allowed; a
replacement is submitted before the old stop is cancelled. Kraken `sendorder` has no attached SL, so it is a separate `stp` order. Design and the
parallel task split (A gateway, B simulated exchange, C dashboard: Codex; D execution lifecycle:
Claude) in `docs/codex/STOP_LOSS_DESIGN.md`. Task 0 done: `ExecutionOrder::position_stop_price`
with validation, optional `position_stop_price` in the service wire and the Postgres snapshot,
temporary guards (gateway rejects locally `POSITION_STOP_UNSUPPORTED`, simulated exchange
rejects) so a position stop can never become a market sell; new
`validation/position_stop_contract_test.cpp`. Checks: 18 component tests PASS, full build,
venue adapter test 110 checks PASS, paper_comparison OK, Step59 PASS fingerprint 94fdf8d8...8da2.
Not deployed.

## VPS release 20261017_145336-9bed096-dirty: Collateral card (2026-10-17, deployed)

User request: show that BTC is collateral, not a position, and remove the "Drop to
Maintenance" card; publish again (authorized). dash-api, LIVE overview: the "Cash" card becomes
"Collateral" = Kraken USD balance (monitor `usdBalance`), BTC quantity and its value after
haircut, and the BTC share of both, e.g. "$102.15 USD · 0.0015458052 BTC ($124.68) · 55% BTC"
(without a fresh monitor reading: "BTC $x (USD balance unavailable)"); the separate
"In BTC Collateral" and "Drop to Maintenance" cards are gone (Margin Headroom stays). Go tests
OK (TestKrakenCollateralCardsOnlyInLive updated), dashboard source audit PASS. Built 14:53 UTC
(dash-api 27aef3ff..., runtime 8f902ca8..., dash-web 355ae68e...; release.json equal),
activated 14:54 UTC from 20261017_142931 (rollback target): 13 containers healthy,
reconciliation clean (cash 40.05 USD, 3 positions, 0 orders), LIVE ledger unchanged; release
20261016_095921 and unused images removed; disk 8.3 GB (15%).

## VPS release 20261017_142931-9bed096-dirty (2026-10-17, deployed)

User explicitly authorized the release, removal of old images/releases and the launch. Contents:
host monitor watching gateway-kraken in LIVE, dash-api Kraken marks / funding / total equity
with the BTC share, Telegram active-alert summaries, comparison one-shadow-attempt-per-day,
research options (`--rebalance`, `--ranker`, `--set`, `--btc-filter-length`, timed strategies),
LIVE strategy/portfolio configs. Pre-release (WSL): build, 17 component tests,
chain_installation_root, host_monitor, paper_comparison 15, standby, kraken_account_monitor,
daily_service_logs OK; go vet + all Go tests OK; dashboard source audit PASS (33 markers);
Step59 PASS with fingerprint 94fdf8d8...8da2. Built 14:28-14:29 UTC (runtime e233a717...,
dash-api 7dc222af..., dash-web 0cbbae59...; release.json IDs equal), SHA256SUMS OK, activated
14:31 UTC from 20261016_095921 (rollback target). After: 13 containers healthy; monitor
restarted, host.json processes all RUNNING including gateway-kraken (no gateway/sim-exchange);
comparison restarted; reconciliation clean (cash 40.05 USD, 3 positions, 0 orders; Kraken USD
102.15 after fees, positions entered 62.09, collateral 124.64); gateway "real order submission
ENABLED", namespace kraken2; LIVE ledger unchanged (3 fills, 3 open, same hashes).
Cleanup: removed releases 20261013_193943, 20261013_212613, 20261014_175134, 20261014_200715,
20261014_201725, 20261015_090303, dashboard-mobile-20261014 and pre-root-20261014_201026 (kept
current and previous); `docker image prune -a` reclaimed 1.28 GB, `docker builder prune -a`
2.13 GB. Disk 14 GB -> 8.2 GB used (15%). Rollback to 20261016_095921 reloads its images.tar.
Standby `sync` to the new release.

## VPS disk growth 2026-10-14..17 explained (2026-10-17, local fix)

User question: disk use rose steadily from about 20% to 38% until the LIVE switch. Cause:
`deploy/chain/comparison.py` (PAPER mode) ran `kraken_shadow.py` every 60 s until the day's
public plan observation was complete; each run stores a new `raw/<timestamp>/` capture of
Kraken's instruments and tickers (about 1 MB), and while a plan stayed unobservable it retried
all day: `data/kraken_shadow/paper-forward` grew 6.2 GB (2026-10-16) -> 7.5 GB (2026-10-17),
about 1.4 GB a day. The steps of the chart were releases and their activation backups
(backups/ was 1.2 GB). Deleted with the PAPER data today; LIVE (`collect_live`) never runs the
shadow tool, and no process, cron or timer writes there now (checked read-only). Fix for PAPER:
one attempt per decision day (the marker records `unavailable`). Remaining growth to watch, read
13:35 UTC: releases/ 2.2 GB (7 release folders of ~260 MB plus pre-root and mobile copies),
containerd 4.3 GB and Docker 2.2 GB of images (765 MB reclaimable) and 2.6 GB of build cache
(1.7 GB reclaimable); logs are bounded (two days / 50 MiB); data/ is 70 MB. The host monitor
already turns HostResources WARN at disk >= 85% or < 15 GiB free (about 74% of 58 GB) and
CRITICAL at >= 95%; the summary label is "VPS CPU, RAM or disk high". paper_comparison_test
15 OK; notifier tests OK.

## Telegram active-alert summaries (2026-10-17, local)

User request: whenever a chat gets an alert update, also post the alerts still active in that
chat (critical group: CRITICAL only; normal chat: the rest), as a count and bullet list of
short names. New `internal/notifier/summary.go`: `SummarySink` (Telegram: `SendSummary`,
silent, same chat routing; `sendMessage` extracted from `Deliver`), `Engine.SetSummarySource`
(the notifier's source name, set from `DASHBOARD_NOTIFIER_SOURCE_NAME`: Watchdog / Kraken).
After a sweep that delivered an alert change (not the daily portfolio), each changed chat gets
"📋 <source> · Urgent alerts now: N" (critical group) or "Active alerts now: N" with
"• <label>" lines, or "✅ All clear"; with no critical group one combined list marks
"(urgent)". Listed: latest event not resolved and already reported (a CRITICAL alert inside
its delay or below the minimum severity is not listed). Labels: `alertLabel` maps event
types to short names ("gateway-kraken down", "NEARUSDT: order rejected", "reconciliation
blocked", "Kraken account unreadable", ...), otherwise the alert's short title. A failed
summary never fails the sweep (logged "active-alert summary not sent"). Each notifier lists
its own source. Tests: TestActiveAlertSummaryPerChat, TestSingleChatSummaryMarksUrgentAndSkipsUnreported;
all Go tests OK, gofmt clean. Not deployed (goes with the next release).

## Kraken LIVE dashboard fixes: marks, funding, total equity, gateway status (2026-10-17, local)

User report after the first real orders: (1) no funding shown, (2) BTC collateral shown apart
from equity, (3) PnL disagrees with Kraken, (4) gateway shown as down. Causes and changes:
- Gateway "down": `deploy/chain/monitor.py` always watched the PAPER services (gateway,
  sim-exchange), so in LIVE both were STOPPED in host.json (and raised CRITICAL process alerts).
  Now the default selection follows MODE in the chain.env next to the script
  (`default_services`): LIVE watches gateway-kraken (`algotrading_exchange_gateway`) and no
  sim-exchange; `--services` still overrides. dash-api's host telemetry list knows
  gateway-kraken. The monitor unit must be restarted after a mode switch.
- PnL: LIVE positions were marked at the latest Binance daily close (NEAR 5.067, the
  2026-10-16 close). New `real_kraken_marks.go`: Kraken public tickers through the symbol map,
  source price = venue mark x fixed scale, cached 15 s; Binance close only as fallback;
  `priceSource` shown in the Active Positions title. PnL and Net PnL were equal because Kraken
  fees arrive from the account history every 300 s after the fill (fills carry commission 0).
- Funding: the runtime already books funding per position (FundingPayment, position ledger);
  dash-api now sets `fundingUsd` and `fundingAvailable` in LIVE, adds funding to Net PnL
  (unrealized + realized - fees + funding), and shows Kraken's current rate per position
  (`fundingRateHourlyPct`/`fundingRateAnnualPct` = fundingRate / mark; + = longs pay; the
  public `fundingRate` is USD per venue unit per hour, e.g. BTC 0.60 at 81,400 = 0.00074%/h).
  Positions page: Funding Rate column (%/y, %/h on hover, red when the position pays) and
  Total Funding card.
- Equity: in LIVE the valuation counts the BTC collateral after haircut (the risk service's
  sizing capital), so Total Equity, current weights and leverage use it; the "BTC Collateral"
  and "Sizing Capital" cards are replaced by "In BTC Collateral" (share of Total Equity); the
  daily equity curve adds each day's `collateral_usd`.
Checks: host_monitor_test 4 OK (1 Docker test skipped); go vet and all Go tests OK
(golang:1.23-alpine, --network none; new TestKrakenMarksUseTheSymbolMapScale and
TestLiveValuationCountsCollateralAndFunding, updated TestKrakenCollateralCardsOnlyInLive);
dash-web (tsc -b + vite) and dash-api images build. Not deployed: needs a release, then
`systemctl restart algotrading-monitor`.

## VPS switched to Kraken LIVE, real orders enabled (2026-10-17)

User request: remove all PAPER history from the VPS except CPU/RAM/disk telemetry, and run
the chosen LIVE strategy with test thresholds RSI(8) > 50 / < 40 until overbought markets
return. Steps (explicitly authorized; the user allowed the remote writes after one
permission-classifier block):
- Kraken read-only check at 12:12 UTC: 0 open orders (the test limit order was cancelled),
  0 positions, USD 102.1695, BTC 0.0015458052.
- New configs `config/strategies/pure_rsi_8_50_40_volume_rank.json` (ranker SMA QuoteVolume
  25) and `config/portfolio/pure_rsi_equal_weight_rel25.json` (equal weight 0.10,
  relative_threshold 0.25), checked locally with the fast baseline, installed in
  /algoTrading/config.
- Comparison service stopped, `algo down` 12:13 UTC; PAPER archive downloaded to
  `storage/vps_paper_archive/vps_paper_archive_20261017.tar.gz` (14.3 MB, 1,623 entries, SHA-256
  44bc7a32...e915f equal on both sides).
- Deleted on the VPS: data/postgres, nats, market, venue, manual, comparison, kraken_shadow
  (7.5 GB), dashboard alerts/audit/notifier/watchdog, every chain log folder except
  kraken-notifier, all backups (1.2 GB) and reports; log receiver restarted. Kept:
  data/telemetry, kraken_account, receiver, dashboard/kraken-notifier, logs/kraken-notifier,
  STATE_FORMAT, config, secrets, releases. Disk 22 GB -> 13 GB used.
- chain.env (copy in backups/chain.env.before-live-20261017): MODE=live, the two new files,
  RSI_ENTRY=50, RSI_EXIT=40, appended LIVE_INITIAL_CASH=102.1695, LIVE_ACCOUNT_NAMESPACE=kraken1,
  LIVE_ORDER_SUBMISSION=disabled, LIVE_DEAD_MANS_SWITCH_SECONDS=0, LIVE_COMMISSION_RATE=0.0005;
  PAPER_COMPARISON_FILE kept.
- `algo up` 12:50 UTC: 13 containers healthy (gateway-kraken instead of gateway/sim-exchange),
  fresh state in data/live. Reconciliation clean: cash 102.17 USD, 0 positions, 0 open orders,
  collateral 124.56 USD after haircut. Market data downloaded the 2026-10-16 day (100 ranked,
  50 tracked); strategy started from a fresh signal state (100 warm-up days) with 3 active
  signals; risk sized on 226.73 USD; planner: BUY NEAR, STRK, WLD 22.67 USD each; the gateway
  rejected all three locally ("order submission disabled"); Kraken still 0 orders/positions.
  Comparison restarted ("Kraken LIVE comparison refreshed"); notifier TELEGRAM healthy;
  standby `sync`: MODE=live with the new files.
First real orders (user's explicit authorization, 2026-10-17: enable submission and relaunch
so yesterday's decision is executed): 12:58 UTC `algo down`, `data/live` removed (one hour old,
only the three rejected orders), LIVE_ACCOUNT_NAMESPACE=kraken2, LIVE_ORDER_SUBMISSION=enabled,
`algo up`. Startup reconciliation clean (102.17 USD, 0 positions, 0 orders); the 2026-10-16
decision was replanned (BUY NEAR, STRK, WLD 22.67 USD each) and executed at market, taker:
4 PF_NEARUSD at 5.0535, 305 PF_STRKUSD at 0.06631, 43 PF_WLDUSD at 0.5036 (venue step
rounding down: 4.47 -> 4 NEAR, 305.98 -> 305 STRK, 43.88 -> 43 WLD). Execution cash 40.08 USD,
3 open positions; Kraken read-only: 0 open orders, 3 long positions of those sizes. Fills carry
commission 0; Kraken fees/funding are booked from the account history every 300 s. STRK filled
10.5% below the Binance 2026-10-16 close (0.0741) because it ran at 13:00 UTC, not at the open.
Earlier account fills (PF_XBTUSD, PF_HYPEUSD) were recorded as pre-existing, not attributed.
Comparison service restarted. From now on each 00:00 UTC cycle trades automatically.

## User decision: LIVE strategy (2026-10-17)

PureRSI RSI(8) > 80 / < 65, top 20 and ranking by SMA(quote volume, 25), max 10 signals,
10% equal weight with a 25% relative rebalance (resize to 10% below 8% or above 13.3%), no
volatility target, no BTC filter. Recorded in docs/ROADMAP.md (top of Next steps) with what the
chain needs; the config files are not created yet.

## PureRSI 8/80/65, 10% equal weight: relative rebalance thresholds (2026-10-17)

Same setup as the variants below (volume ranking, Binance perps 2020-2026, fee 0.05%),
`--sizer equal-weight --rebalance relative --rebalance-threshold 0.25|0.5|1.0|1.5`. The rule
compares |10% - current weight| with threshold x current weight, so it is asymmetric:
20% resizes below 8.3% or above 12.5%, 25% below 8% / above 13.3%, 50% below 6.7% / above
20%, 100% only below 5%, 150% only below 4% (winners never trimmed). Final USD / CAGR / max
DD / vol / Sharpe / Calmar: none 2,151,606 / 56.2% / 46.7% / 46.5% / 1.20 / 1.20; 20%
2,538,532 / 60.0% / 30.8% / 35.5% / 1.50 / 1.95; 25% 2,888,259 / 63.0% / 30.5% / 36.2% /
1.53 / 2.07; 50% 3,056,657 / 64.4% / 32.0% / 38.8% / 1.47 / 2.01; 100% and 150% equal to no
rebalance (2,151,501 / 2,151,606). The gain comes from trimming winners. Page:
`storage/backtests/sensitivity_results/pure_rsi_comparison_binance_perps_2020/PureRSI_8_80_65_ew10_rebalance_thresholds.html`.

## PureRSI 8/80/65: sizing and rebalance variants (2026-10-17)

User request: PureRSI RSI(8) > 80 / < 65, always ranked by SMA(volume, 25), four variants
plus the current one with a BTC filter. `algotrading_research_html --rebalance` now applies
to both sizers (`none|absolute|relative`; default none for equal weight, absolute for
volatility target, so earlier commands keep their behaviour), and `--btc-filter-length N`
gives PureRSI a `BenchmarkAboveSMAFilter` on new signals (study option; PureRSI CSV columns
unchanged). Studies `pure_rsi_8_80_65_*_volrank_binance_perps_2020_fee0.0005`. Final USD /
CAGR / max DD / realized vol / Sharpe / Calmar:
- EW 10%, no rebalance: 2,151,606 / 56.2% / 46.7% / 46.5% / 1.20 / 1.20
- EW 10%, 20% relative rebalance: 2,538,532 / 60.0% / 30.8% / 35.5% / 1.50 / 1.95
- VT 20%, no rebalance: 699,648 / 32.7% / 19.1% / 22.7% / 1.36 / 1.71
- VT 20%, 20% relative rebalance (deployed rule): 590,818 / 29.5% / 21.3% / 22.2% / 1.27 / 1.38
- the same with BTC > SMA50 for new signals: 863,397 / 36.8% / 27.9% / 29.3% / 1.20 / 1.32
- reference BTC held while close > SMA50 (close-to-close, 0.05% per switch): 2,165,703 /
  56.4% / 58.1% DD; BTC buy and hold +1,013%, DD 77%.
Page: `storage/backtests/sensitivity_results/pure_rsi_comparison_binance_perps_2020/PureRSI_8_80_65_sizing_variants.html`.
After the `--rebalance`/`--btc-filter-length` and `btcFilterLength` changes: full build,
research_html_report_gate PASS, research_scenario_gate PASS.

## BargainChaser and MRRSILong with a BTC uptrend filter (2026-10-17)

User request: no new entries unless BTC close > SMA(50). New parameter `btcFilterLength`
(default 0 = no filter, so earlier results are unchanged) in both shared definitions; it
adds the library's `BenchmarkAboveSMAFilter(benchmark, length)` through new optional
`marketFilters` arguments of `TimedResearchStrategy`, `BargainChaserStrategy` and
`RSIMeanReversionStrategy` (the base class already checks market filters before entries;
open positions still exit after their bars). Run with `--set btcFilterLength=50`, 10% equal
weight, volume ranking, same data: BargainChaser 265,195 -> 244,401 USD (CAGR 15.2% ->
13.9%, max DD 31.4% -> 23.7%, Calmar 0.48 -> 0.58, exposure 32% -> 19%; 2024 -10.8 -> -7.4,
2026 -17.1 -> -5.8); MRRSILong 121,214 -> 103,073 (CAGR 2.8% -> 0.4%, max DD 28.4% ->
30.1%). Added (dotted) to `All_strategies_equal10_volume_rank.html`.

## All strategies, 10% equal weight, ranked by SMA(volume, 25) (2026-10-16)

User request: every strategy with equal weight and, except PureMom, candidates ranked by
SMA(quote volume, 25) instead of their own ranker. New `algotrading_research_html --ranker
own|volume` (default own): `research_studies::makeStudyRanker` replaces the ranker of every
HTML and shared definition except PureMom when `volume` is chosen. `--strategy ATRBreakout`
and `MRShort` are now selectable, with the multi-strategy values (ATRBreakout 3 bars / ATR(10)
x 0.875, matching the August freeze; MRShort RSI(5) > 65, BTC < SMA50, entry close - 0.3 ATR(10),
7 bars); MRShort also passes the benchmark symbol. Studies `*_equal10_volrank_binance_perps_2020_fee0.0005`.
Sorted by CAGR/vol: XHBreakout 1.47 (40.3% CAGR, DD 33.1%); PureRSI 7/80/70 1.44 (57.4%,
31.4%); XHBreakout_ATR 1.21 (51.2%, 43.3%); PureRSI 8/80/65 1.21 (56.2%, 46.7%); Donchian
1.20 (76.9%, 50.4%); ATRBreakout 0.97 (22.6%, 49.1%); MRShort 0.74 (20.9%, 27.5%, +45.8% in
2025 and +74.4% in 2026); BargainChaser 0.56; PureMom 0.15; MRRSILong 0.13. MRShort is
negatively correlated with every long strategy (-0.08 to -0.22). Against each strategy's own
ranking (equal weight): PureRSI 7/80/70 2.13M -> 2.27M USD, Donchian 7.50M -> 5.07M,
BargainChaser and PureMom unchanged in practice, MRRSILong 139k -> 121k. After all research
changes (including `--ranker`): full build, research_html_report_gate PASS,
research_scenario_gate PASS.
Page: `storage/backtests/sensitivity_results/pure_rsi_comparison_binance_perps_2020/All_strategies_equal10_volume_rank.html`.

## All research strategies on Binance perpetuals 2020-2026 (2026-10-16)

User request: compare DonchianBreakout, PureMom, BargainChaser and MRRSILong with their
research defaults against PureRSI, each with the 20% vol target and 10% equal weight
(same data/window/fee). `algotrading_research_html --strategy PureMom|BargainChaser|MRRSILong`
now takes them from `research_studies::makeStrategyDefinitions(benchmark)` (only when named,
`all` unchanged); `makeStrategyDefinitions` gained the PureMom BTC-filter symbol (default
"BTC", so BTCUSDT works with Binance data); BargainChaser uses the multi-strategy study's
1-bar exit (the shared definition had 2). Defaults: Donchian lookback 30, no market filter,
ROC 30 ranking; PureMom ROC 7, 7 bars, BTC > SMA50, max 3 positions; BargainChaser fall
> 10% with close > SMA50, ROC(1) ascending, 1 bar; MRRSILong RSI(3) < 10, ROC 30 ranking,
1 bar. Sorted by CAGR/realized vol: PureRSI 7/80/70 VT 1.75 (37.0% CAGR, DD 21.3%);
PureRSI 7/80/70 EW 1.40 (56.0%, 31.4%); Donchian EW 1.34 (87.3%, 51.2%, exposure 84%);
PureRSI 8/80/65 VT 1.27; PureRSI 8/80/65 EW 1.20; Donchian VT 0.92 (25.1%, 29.7%);
BargainChaser EW 0.55; MRRSILong EW 0.24; PureMom EW 0.15; PureMom VT, BargainChaser VT,
MRRSILong VT about 0.1 (CAGR 1-2%). Daily-return correlation with PureRSI VT: Donchian 0.52,
PureMom 0.38, BargainChaser 0.07, MRRSILong 0.03. Page:
`storage/backtests/sensitivity_results/pure_rsi_comparison_binance_perps_2020/All_strategies_comparison.html`.

## PureRSI sizing comparison: 20% vol target vs 10% equal weight (2026-10-16)

Same data/window/fee as the two studies below; equal weight = default `--sizer equal-weight`
(10% per signal, entry/exit-only), studies `pure_rsi_8_80_65_equal10_*` and
`pure_rsi_80_70_7_equal10_*`. Final USD / CAGR / max DD / realized vol / Sharpe / Calmar:
8/80/65 VT 544,150 / 27.9% / 21.2% / 22.0% / 1.23 / 1.32; 7/80/70 VT 873,438 / 37.0% /
21.3% / 21.1% / 1.59 / 1.74; 8/80/65 EW 2,093,721 / 55.6% / 46.7% / 46.4% / 1.19 / 1.19;
7/80/70 EW 2,133,710 / 56.0% / 31.4% / 40.1% / 1.30 / 1.79. Equal weight carries about twice
the volatility; per unit of volatility 7/80/70 VT is best (CAGR/vol 1.75). Chart and tables:
`storage/backtests/sensitivity_results/pure_rsi_comparison_binance_perps_2020/PureRSI_sizing_comparison.html`.

## PureRSI 8/80/65 (user decision for LIVE), same backtest (2026-10-16)

User decision 2026-10-16: RSI length 8, entry > 80, exit < 65 (candidate #1 of
`PureRSI_ValidP.docx`); it replaces the 7/80/70 of the TO-DO table. `algotrading_research_html`
gained `--set name=value` (repeatable; overrides baseline parameters of the selected strategy,
unknown names and invalid combinations rejected; grid ranges unchanged). Same data and
options as the 7/80/70 run below plus `--set rsiLength=8 --set rsiEntry=80 --set rsiExit=65`,
study `pure_rsi_8_80_65_voltarget20_rel20_binance_perps_2020_fee0.0005`: 100,000 ->
544,150 USD (+444%, 27.9% annualized), max DD 21.2% (710 days), Sharpe 1.23, Sortino 2.13,
Calmar 1.32, 560 trades, win rate 41%, PF 1.48, exposure 62%. Calendar years (8/80/65 vs
7/80/70): 2020 +85.1/+83.2, 2021 +78.8/+98.4, 2022 +13.8/+8.5, 2023 +51.2/+53.5,
2024 -6.7/+8.2, 2025 +11.1/+27.1, 2026 -7.7/+4.9. Interactive chart with both and BTC:
`reports/PureRSI_equity_interactive.html` of the study (also one for 7/80/70).

## PureRSI 80/70/7, 20% vol target, 20% relative resize, Binance perpetuals 2020-2026 (2026-10-16)

User request: backtest of all Binance with the user's frozen parameters (RSI 7, entry 80,
exit 70, ranker RSI 7; source: `algoTrading - copia/TO-DO.txt`, 2026-08-10). Data:
`tools/binance_perp_daily.py --start 2019-11-01` ->
`storage/databases/1d_binance_perp_quote_20191101.csv` (654 USDT perpetuals with data,
2019-11-01..2026-10-15; symbols per year 3/69/113/133/228/357/601/654 for 2019..2026).
`algotrading_research_html` gained `--rebalance absolute|relative` (default absolute,
unchanged), so the study matches the deployed portfolio file. Run: `--start 20191201
--end 20261015 --benchmark BTCUSDT --sizer volatility-target --target-volatility 0.20
--rebalance relative --rebalance-threshold 0.20 --fee-taker 0.0005`, study
`pure_rsi_80_70_7_voltarget20_rel20_binance_perps_2020_fee0.0005`. Result: 100,000 ->
873,438 USD (+773%, 37.0% annualized), max DD 21.3% (492 days), Sharpe 1.59, Sortino 2.99,
Calmar 1.74, 761 trades, win rate 38%, PF 1.61, exposure 62%. Calendar years: 2020 +83%,
2021 +98%, 2022 +8.5%, 2023 +54%, 2024 +8.2%, 2025 +27%, 2026 (to 15 Oct) +4.9%. BTCUSDT
over the same window +1,013% with 77% max DD. Monte Carlo of the closed trades (seeded
bootstrap without order) shows 30% DD in 63% of paths. Not modelled: funding, slippage,
contracts already removed from exchangeInfo (survivorship, strongest in 2020-2022), Kraken
availability. The option is opt-in (default path unchanged); research_html_report_gate PASS
after the change.

## VPS release 20261016_095921-9bed096-dirty (2026-10-16, deployed)

User explicitly authorized the release and the follow-up VPS steps. Built 09:59 UTC
(runtime 4f8eb8de..., dash-api a4cb58fd..., dash-web dc5b6c4b...; release.json IDs equal
the fresh builds); uploaded, SHA256SUMS OK, activated 10:01-10:03 UTC from 20261015_090303
(rollback target). Contents: equity-row fix, Hyperliquid gateway mode removal, dashboard
chain boundary, repo cleanup, 20% relative rule (config file only; the VPS portfolio file
still has the absolute 0.02 threshold until adopted), heartbeat files (installed units
unchanged, no NOTE), chain_profile/standby sources, comparison-baseline top-50 fix and
the 5m critical delay of the general notifier. After activation: 14 containers healthy,
heartbeat timer active, risk restored its checkpoint (last decision 2026-10-15); fills
25, open 3, closed 8, simulated positions and cash hashes unchanged (only today's
reconciliation check count 9 -> 11 from the restart rerun). Equity rows before repair:
20261013 cash 77,252.91 / equity 92,195.40 and 20261014 equity 91,694.59 (overwritten).
Follow-up VPS steps (10:45-10:52 UTC, after the user allowed the remote writes):
- Equity rows: `algo backup` (backups/20261016_104509.tar.gz), DELETE 2, restart execution:
  "Backfilled 2 days of equity history from the fills"; 20261013 cash 45,535.43 / equity
  92,230.58, 20261014 equity 91,699.43 (= the VPS sizing capital); 20261015 kept.
- Portfolio: copies of the file and chain.env in backups/ (`*.20261016`); rebalance set to
  relative_threshold 0.20; RISK_ADOPT_PORTFOLIO_CONFIG=yes, up (risk ALERT "Portfolio
  config changed on purpose"), back to no, up (risk ready), comparison restarted (previous
  study archived as archive-6ec3b412f06b, new fingerprint eebaabf1...).
- Notifier: NOTIFIER_SINK=TELEGRAM (same up); starts with sink TELEGRAM, criticalDelay 5m,
  store data/dashboard/notifier/telegram. Its bootstrap over 14,943 old events needed five
  10 s sweep timeouts (progress is kept per event), then completed: 14,931 history events
  suppressed and one active alert sent (WARN "Risk decision checkpoint unavailable").
- That WARN is real and expected until the next decision: the adopted risk checkpoint now
  carries the new portfolio identity while the stored 2026-10-15 risk diagnostics carry the
  old one, and dash-api's decodeRiskDiagnostics requires equality. Trading is unaffected
  (risk healthy and ready); it should resolve with the 2026-10-16 close decision at
  00:00 UTC. Possible later improvement: show older-identity diagnostics as such instead of
  failing the read.
- Standby `sync` from Windows: release 20261016_095921, portfolio relative_threshold 0.20.
All 14 containers healthy after the steps.

## General watchdog alerts to Telegram, CRITICAL after 5 minutes (2026-10-16, local)

User decision: every VPS watchdog alert goes to Telegram (CRITICAL to the critical group,
the rest to the normal chat, the existing `DASHBOARD_TELEGRAM_CRITICAL_CHAT_ID` routing),
and a CRITICAL one only once it has lasted about 5 minutes. Read-only look at the VPS test
sink (`data/dashboard/notifier/test-sink.jsonl`, 14,413 lines since 2026-10-13): each
restart on 2026-10-15 opened and resolved about ten CRITICAL alerts within a minute
(TRADING_PROCESS_UNAVAILABLE per service, SOURCE_CRITICAL DailyTradingProgress/PostgreSQL).
New notifier option `DASHBOARD_NOTIFIER_CRITICAL_DELAY` (`Engine.SetCriticalDelay`,
default 0, so kraken-notifier is unchanged; 5m for the general notifier in
deploy/chain/compose.yml): a not yet reported CRITICAL alert is left unprocessed until it
has been active that long from the start of its episode; resolved earlier, it and its
recovery are suppressed (RESOLVED_WITHIN_CRITICAL_DELAY); later events of a held alert
wait with it; WARN, recoveries of reported alerts and the bootstrap of active alerts are not
delayed; an unreadable time sends at once. The sweep log gains `deferred`. New Go test
TestCriticalDelayHoldsBriefOutagesAndReportsLongOnes. Checks: gofmt clean on the changed
files, go vet and all Go tests OK (golang:1.23-alpine, --network none),
chain_installation_root_test OK, verify-project-contracts OK.
VPS pending (authorization): release, then `NOTIFIER_SINK=TELEGRAM` in
`/algoTrading/chain/chain.env` and restart only the notifier; its first Telegram start
sends the alerts active at that moment once.

## Comparison baseline: held coin outside the top 50 (2026-10-16, local, not deployed)

The in-process TradingEngine used one MarketData for signals and for sizing/rebalance
prices. The comparison baseline (`research/src/common/paper_baseline.cpp`) fills it with
the day's top-50 entry universe, so a coin still held or signalled after leaving the top 50
had no close(T): the threshold rules abort ("Current price must be positive") and the
volatility sizer lacks its bar. The LIVE chain is not affected: its risk service loads every
signalled and held symbol. Fix: `DecisionEngine::onBarClose` / `TradingEngine::onBarClose`
gain a split form (signalData for `updateSignals`, sizingData for
`calculateRebalancePlan`); the old one-argument form passes the same data twice, so the
backtester, fake-live tool and canonical replay are unchanged. The baseline keeps its
signal input as before and adds close(T) of every signalled/held coin to the sizing input,
as the risk service does. New paper_comparison_test case (held ETH with an active signal,
absent from the ranking): the old binary aborted (SIGABRT); now it is priced and resized.
Checks (WSL): full build; 17 component tests; paper_comparison_test 15 OK;
paper_trading_test PASS; standby_test OK; research_html_report_gate and
research_scenario_gate PASS; Step59 PASS with the unchanged fingerprint 94fdf8d8...8da2.

## Local standby, phase 1: the VPS's next orders computed locally (2026-10-16, local)

User request (ROADMAP Next steps 5): if the VPS is down, compute here what the chain would do.
New `standby/` (README there): `standby.py sync` copies the deployed profile read-only over
SSH (MODE, STRATEGY_FILE, PORTFOLIO_FILE, LIVE_COMMISSION_RATE lines of chain.env, those two
config files, the Kraken symbol map, the current release id) into ignored
`standby/profile/`; `standby.py plan` downloads 130 daily klines of every Binance USDT
perpetual with status TRADING into the market-data SQLite schema (ohlcv_data + top-100
quote-volume ranking per day), reads the Kraken account with the read-only key (or
`--account-file`), runs `build/standby/algotrading_standby_plan` and prints the orders with
Kraken symbol and contracts; it never sends (`"sent": false`). The planner is the chain's
service split with the canonical replay's components: StrategySignalEngine over 30 settle
days + T with the strategy service window (top 50 + active signals, 100 days),
PortfolioRiskEngine at close(T) with the real account and the risk service window
(signalled + held symbols, lookback + 5 days), NotionalOrderPlanner at the exact close(T).
The account mapping follows live_venue_adapter.cpp (cash = USD balance - entry value,
source units, collateral after haircut). A first version on the in-process TradingEngine
failed when a held coin left the day's top 50 (that engine prices from the top-N market data
only); the same gap in the comparison baseline was fixed afterwards (section above). New `research/src/common/chain_profile.*`: profile builders shared by the
baseline (moved from paper_baseline.cpp unchanged) and the standby (strategy/risk builders
mirroring the service loaders).
Validation against the VPS (comparison manifest, read-only; PAPER account before each
decision): closes 20261013/14/15 give the VPS's sizing capital (92,230.58 / 91,699.43 /
91,992.67), signals, actions/target weights and order amounts to the cent (for example
20261014 sell AVAX 2,926.81 and ONDO 2,842.87 USD; 20261015 buy 1000PEPE 1,932.50 and WLD
1,890.03). `sync` ran from Windows (WSL ssh had no VPS host key). Checks (WSL): full build;
new validation/standby_test.py (Kraken account mapping, unmapped symbols) OK;
paper_comparison_test 14 OK after the chain_profile move; chain_installation_root OK;
research_html_report_gate PASS; Step59 PASS with the unchanged fingerprint 94fdf8d8...8da2.
Pending: phase 2 (sending orders from local) once the chain trades Kraken LIVE; run `sync`
after each release.

## 20% relative resize rule (2026-10-15, local, not deployed)

User decision: a held position is resized only when its target weight differs from its
current weight (quantity x current price / strategy capital) by at least 20% of that
weight; entries, exits and side changes always happen. Before: the deployed
`pure_rsi_vol_target.json` used an absolute 2-point `threshold` (0.02). New header-only
`lib/src/rebalance/relative_threshold_rebalance_policy.h` (`RelativeThresholdRebalancePolicy`),
config type `relative_threshold`, read by portfolio_risk_service and the fast baseline
(`research/src/common/paper_baseline.cpp`, used by comparison.py), so PAPER and its
"Expected" line apply the same rule. `config/portfolio/pure_rsi_vol_target.json` now has
`{"type": "relative_threshold", "threshold": 0.20}`. The HTML study's
`--rebalance-threshold` keeps its absolute meaning. New component test
relative_threshold_rebalance_test (10% holds in (8%, 12%), 5% in (4%, 6%), shorts, price
drift, entries/exits/reversals). Checks (WSL): full build; "All 17 component tests pass";
paper_comparison_test 14 OK (runs the baseline with the new file); chain_installation_root
OK; Step59 PASS with the unchanged fingerprint 94fdf8d8...8da2 (the canonical replay uses
entry/exit-only rebalancing). VPS: needs the release (risk/baseline must know the type), then
editing /algoTrading/config/portfolio/pure_rsi_vol_target.json and the adopt procedure of
deploy/chain/README.md "Changing the portfolio sizer"; comparison.py then archives the study.

## Off-host heartbeat for "VPS down" alerts (2026-10-15, installed on the VPS)

Every Telegram alert is sent from the VPS itself, so a host that is down, frozen or
offline sent nothing. New host units `deploy/chain/systemd/algotrading-heartbeat.{service,
timer}`: once a minute one empty outbound HTTPS GET to the user's healthchecks.io check
(Period 1 min, Grace 5 min, Telegram integration; the user created the account and
connected Telegram). healthchecks.io alerts when pings stop and when they return. The ping
URL is the only secret: `secrets/heartbeat.curl` (bare URL or `url = "..."`), passed as a
systemd credential (`LoadCredential`) to `chain/heartbeat_ping.sh`, which hands it to curl
on stdin: never in argv, environment or logs; DynamicUser, no capabilities, ProtectSystem=strict, 32 MB. Independent of Docker and
trading. `algo install` now copies systemd/*.timer and reports differing timers; `algo ps`
shows "off-host heartbeat: running|STOPPED|not configured". README section "Off-host
heartbeat" has the enable commands. Checks (WSL): bash -n algo; chain_installation_root_test
(rendered root paths); `systemd-analyze verify` of the rendered units exit 0 (systemd 249;
VPS has 255 and curl 8.5, read-only check); curl reads the URL from the config file
(local HTTP server saw the path), heartbeat_ping.sh accepts five URL formats (bare,
spaces, quotes, url =, blank line/CR) and rejects a file without https:// without
printing it; a failed ping prints no URL.
VPS (user-authorized, 18:12-18:25 UTC): the user created secrets/heartbeat.curl (never
opened by the agent); the rendered units went to /etc/systemd/system and the script to
/algoTrading/chain/heartbeat_ping.sh (by hand, outside a release; the next release ships
the same files). `systemd-analyze verify` OK, timer enabled; the first attempts failed
(the bare URL was not valid curl config, hence the script), then 18:24:32 and 18:24:34
Result=success, next run every minute. No container restarted (all 14 healthy, up 9 h).
`algo ps` shows the heartbeat line only after the next release (algo itself unchanged on
the VPS). Noted: /etc/systemd/system/algotrading-kraken.service has an executable bit
(systemd warns; harmless, not changed).

## Repo cleanup round A/B1/C/B2/D and equity-row fix (2026-10-15, local, not deployed)

Tasks of storage/cleanup_tasks_20261015.txt; each task's full notes, file lists and
outputs are in storage/cleanup_notes/{A,B1,C,B2}.md. No VPS change, release or commit.
- A (storage and unused files): 229 approved paths removed (old releases, temporary
  profiles/images, old campaigns, finished logs; ~1.98 GiB, storage 9.7G -> 7.7G) plus six
  unused tracked helpers: config/database/database_{config,config_sim,schema}.json and
  tools/historical_replay/{apply_t18_execution_account_snapshot_barrier,
  apply_t18_replay_event_time_semantics,t23_cleanup_current}.py (empty config/database/
  removed in D). Kept: datasets, backtests, Kraken shadow evidence, current and previous
  release, recent PAPER campaigns. The "PREGUNTAR" group of A-inventory.md (Parquet,
  VPS backups/snapshot, tax scripts, historical runner family) is untouched and still
  needs the user's decision. 49 references in older entries of this file name removed
  logs/captures (mostly storage/pipeline_risk_step59.log, storage/*_build.log and
  storage/paper_validation/algotrading-paper-*); their accepted results stand, the files
  are no longer available (list: storage/cleanup_notes/A-reference-audit.json).
- B1, accounting fix: `trading_equity_daily` was upserted, and after every ExecutionState
  restart NATS redelivers the last completed day's market update, so `recordEquityDay(T)`
  rewrote close(T)'s row with the live account that already holds T+1's open fills. Now
  `PostgresStateStore::insertEquityDay` (`ON CONFLICT(date) DO NOTHING`, returns whether it
  wrote): the first row of a day wins, like trading_daily_collateral (fees/funding of T's
  fills arrive minutes after the fill, long before the row is written at ~00:01 T+1). Day
  rules and backfill moved unchanged to execution_state_service/src/equity_history.*;
  Manual Control helpers to manual_requests.*. New equity_history_test (component tests
  now 16) and an `equity` phase in transport_persistence_integration_test against real
  PostgreSQL 16 (that test was broken before: its include list predated contracts/
  position_ledger; fixed). Rebuild with the VPS fills/closes (read-only export):
  20261013 cash 45,535.43 / equity 92,230.58; 20261014 equity 91,699.43 (= original row).
  Two stale source audits (execution_state, strategy time handler) were already failing
  at HEAD and now match the source.
- B1, removals requested by the user: gateway `--mode hyperliquid-dry-run` (and its
  NOTIONAL_ORDER_PLAN consumer); the dashboard Hyperliquid TESTNET path
  (internal/integration/hyperliquid/, provider venue_{foundation,public,rules,
  symbol_mapping}.go, `/api/venue-*`, three readiness requirements, frontend types,
  dashboard/scripts/step33..37, DASHBOARD_HYPERLIQUID_* settings). The real provider
  (API and watchdog) now accepts only PAPER (SIMULATED/PAPER/backend) or Kraken LIVE
  (KRAKEN/LIVE/venue) via `provider.ValidateChainBoundary`, which is what deploy/chain sets.
  The standalone dashboard overlays docker-compose.real*.yml no longer start as they are
  (header says so); proposal: retire that standalone deployment track in favour of
  deploy/chain.
- C (Python and chain scripts): kraken_account.py, kraken_account_monitor.py,
  deploy/chain/monitor.py, logging/clean_daily_logs.py restructured with byte-identical
  outputs (C-equivalence.py: 25 normalization/error cases, monitor bytes/SQLite rows
  through failure/recovery/restart, 32 container states).
- B2 (research/src): the HTML, BTC moving-average and XH studies shared ~1,000 copied lines
  each; now research/src/common/parameter_sweep.* (also used by the initial-parameter study)
  and sensitivity_study.*; scenario_reports.cpp split into scenario_reports + plotly_chart_html;
  dead backtest_helpers functions, a commented-out block and migration wording removed
  (5,834 lines removed from existing files). 15 study runs before/after: 192 CSV/HTML/JSON
  outputs byte-identical. research_html_report_gate.py fixtures follow the new names.
- Found, not fixed: `research/replay.py fast` fails locally ("undefined symbol
  TradingEngine::onBarClose") because it prepends deploy/historical_replay/.runtime_bundle/lib,
  whose libalgolib (2026-10-11) predates the `extraCapital` parameter; with that path
  disabled the 30-day run matches RealTest 1/1. validation/t19_realtest_gate_audit.sh
  requires research/src/realtest.h (moved to research/src/common/ long ago). lib's
  `Backtester::closeTrades()` is an empty stub with no caller left.

Final validation in D (WSL, after every task; full output in the session scratchpad):
full meson build OK; mock_component_tests "All 16 component tests pass"; venue adapter
"PASS (110 checks, 0 failures)"; Python kraken_account 29, kraken_account_monitor 20
(1 optional skipped), kraken_portfolio_summary 6, kraken_shadow 14, paper_comparison 14,
chain_installation_root 1, daily_service_logs 8, host_monitor 3 (1 skipped),
resource_profile 4, exchange_coverage 5, universe_coverage_history 2, ai_index 4,
shadow_trading --unit 5, historical_replay_time_env PASS - all OK;
transport_persistence_integration (real NATS + PostgreSQL 16) rollback/crash, recover,
verify, equity PASS; go vet + go test ./... (golang:1.23-alpine, --network none) all ok;
verify-project-contracts PASS; 14 audits (7 *_time_handler, no_legacy_clock,
live_contract_identity, live_daily_gate, live_deploy_topology, live_pre_exchange_e2e,
live_runtime_clock, live_sqlite_ownership) exit 0; research_html_report_gate
(INITIAL, BTC-MA, XH, XH-ATR, XH-ROBUSTNESS, HTML) and research_scenario_gate PASS;
Step59 PASS, fingerprint 94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25, 0 differences. `.ai` index regenerated, --check PASS.
Repo-wide check: no relative Markdown link points to a missing file; the deleted paths
are named only in history or "removed" notes.

Release: needed. ExecutionState (equity fix), exchange-gateway (mode removed),
dash-api/watchdog (chain boundary, removed endpoints) and dash-web change on the VPS.
VPS repair of the two overwritten rows, after the release is active and away from
23:30-00:30 UTC (needs explicit authorization): `sudo /algoTrading/chain/algo backup`;
`DELETE FROM trading_equity_daily WHERE date IN (20261013, 20261014);` (projection table,
rebuilt from trading_fills); `sudo /algoTrading/chain/algo restart execution`; expect
"Backfilled 2 days of equity history from the fills" in logs/execution, then check
20261013 cash 45,535.43 and 20261014 equity 91,699.43.

## VPS release 20261015_090303-d11699d-dirty (2026-10-15, deployed)

User explicitly authorized build, release and deployment. Contents beyond the running
20261014_201725 release (checked file by file against its source.tar.gz): the
`/fapi/v1/time` day-close guard, task 1 (Kraken monitor) and task 2 (notifier source
warning), plus docs. Pre-release checks: go vet + all Go tests (offline container), Kraken
Python tests 29/19+1/6, venue adapter 110 checks, chain_installation_root_test 1/1, Step59
PASS `94fdf8d8…`. New images runtime `fe1ec77c…` and dash-api `f5f5f9e0…` (09:02 UTC);
dash-web unchanged in content. release.json IDs match the fresh builds; SHA256SUMS OK on
the VPS. `activate` at 09:12 UTC (backup, install, `:chain` moved; previous =
20261014_201725 for `algo rollback`). Only differing host unit: algotrading-kraken.service
(`--unreadable-warning-minutes 10`); copied, daemon-reload, monitor restarted 09:15.
- 09:17 UTC: all 14 containers healthy on the new images. Ledger before/after identical:
  23 fills, 3 open / 8 closed positions (same hashes), simulated cash 83,017.75 USD and
  positions; startup reconciliation clean. Services restored their checkpoints (strategy/
  risk 2026-10-14, execution 83,017.75 USD, sim-exchange); no service warnings or errors.
- Kraken monitor: account history read (38 entries) into account-log-summary.json;
  status lastSuccessAt fresh, lastError empty; kraken-notifier healthy. The 09:00 UTC daily
  portfolio was delivered to Telegram at 09:00:14 (receipt DAILY_PORTFOLIO:2026-10-15) by
  the previous monitor; the restart did not duplicate it.
- The general notifier logged 5 "context deadline exceeded" sweeps at startup while
  catching up 14,8xx source events (once also after yesterday's activation); normal since.
- Finding (pre-existing, not from this release): on every ExecutionState restart the
  redelivered market update of the last completed day calls `recordEquityDay` with the
  live account, which already includes the next open's fills, and overwrites that day's
  `trading_equity_daily` row. 20261014 went 91,699.43 -> 91,694.59 USD (cash 83,017.75 after
  the 10-15 sells + 3 positions at the 10-14 closes); 20261013 was already overwritten at the
  2026-10-14 20:21 restart (cash 77,252.91 instead of 45,535.43). Fills, positions, cash and
  trading are unaffected (risk skips the already decided day). Not fixed: needs an
  accounting change (keep the first row of a day) and repair of the two rows.
- Pending: tonight's 00:00 cycle without the one-minute wait; first hourly account-history
  refresh.

## Task 1: Kraken monitor failure cause, KRAKEN_UNREADABLE and account history (2026-10-15, local)

Task 1 of storage/kraken_tasks_20261015.txt (task 2 below is a separate session). Not deployed.
- `kraken_account.py`: failures name their cause without bodies/keys: "Kraken HTTP 503 at
  wallets", "Kraken timeout|DNS failure|TLS failure|connection failure|network failure at
  <route>", "Kraken invalid response at <route>" (401/403 wording unchanged). Journal refusals
  are `JournalError`. New sixth fixed GET `account_log`
  (`/api/history/v2/account-log?count=500`, query signed as postData like the gateway),
  permission-checked, read apart from the snapshot and its 5 s rule; `account_log_summary()`
  keeps no ids (only whether an execution id exists) plus field names/types and `info` counts.
- `kraken_account_monitor.py`: cause logged on first failure, cause change and recovery.
  Heartbeat contract: `lastSuccessAt` = the monitor worked and published (Kraken answering or
  not), `lastError` only for journal/storage failures. After `--unreadable-warning-minutes`
  (10, explicit in `algotrading-kraken.service`) of failed reads, one WARN KRAKEN_UNREADABLE
  (cause, failing since, last good read); the next good read resolves it; a failed read never
  touches the other alerts. Its alert id is bound to the journal's scope/environment/policy,
  not the account hash, so it works before the first good read and across restarts (journal
  context unchanged; no need to move data/kraken_account). Account history at start and
  hourly (retry after 5 min) into `account-log-summary.json`; its failure opens nothing.
- Real read (user-authorized, read-only key consumed by the program, `--once` from WSL at
  07:55 UTC after Kraken's maintenance, output storage/kraken_shadow/account-log-check-20261015/):
  signing accepted, 38 history rows (the user's small PF_XBTUSD/PF_HYPEUSD shorts of
  2026-10-07 and two BTC transfers). Confirmed: every row carries all 22 fields (null when
  unused); `info` "futures trade" (30), "conversion" (6), "cross-exchange transfer" (2);
  `old_balance`/`new_balance` = balance of the row's asset (usd, xbt or the contract's signed
  size); each trade has a USD row and a contract row; USD new = old + realized_pnl - fee +
  realized_funding (fee paid in USD, positive; funding positive = received, +0.0106 and
  +0.0028 USD on reduced shorts; realized PnL settles at once; opening moves USD only by the
  fee). With an empty USD balance Kraken itself converts BTC to pay the fee ("conversion"
  usd +x / xbt -y, same timestamp): the parser books CAPITAL_FLOW +x and Fee -x, net zero.
  Not confirmed: interest and plain USD deposit words, `id` integer type (summary now
  records types), `execution` = `/fills` id, `openPositions[].price` = average entry,
  periodic funding rows while open, scaled-asset units.
- New gateway parser test "real account history shape (2026-10-15)" (synthetic ids, real
  words/amounts): 4 booked entries, 2 collateral-only, none unaccounted, booked + realized
  PnL = balance changes. Parser logic unchanged; its comments lose UNCONFIRMED where confirmed.
- Checks (WSL): kraken_account_test 29/29 (new: account_log signing/permission/shape,
  summary without ids with types, five failure causes); kraken_account_monitor_test 19 pass
  + 1 optional (new: 9.5 min no warning, 10 min one warning, resolved by a good read,
  reopened later, before the first read + restart without duplicate, cause logged only on
  change and recovery, journal failure sets lastError and stops lastSuccessAt, hourly
  history and a failed history changes nothing); the optional Docker contract test with the
  real Go notifier (TEST_FILE, --network none) also delivers KRAKEN_UNREADABLE and its
  resolution; kraken_portfolio_summary_test 6/6; full meson build; venue adapter 110 checks
  (21/21 tests); Step59 PASS with the unchanged fingerprint `94fdf8d8…` (RealTest 25/25).

## Task 2: notifier warns when its alert source stops (2026-10-15, local)

Local-only Go/Compose/documentation change requested independently of the Python
Kraken monitor task. The notifier now owns a separate source-health incident:
missing heartbeat/success, success older than 90 s, success more than 30 s ahead,
lastError, or unreadable/invalid source evidence starts a continuous-failure timer.
At 300 s it sends one WARN "Alert source stopped", naming Watchdog or Kraken,
the first observed failure time and a fixed safe reason (missing, stale,
ahead of clock, source error). Raw source errors/paths never enter that message.
Recovery sends one resolved notification; short interruptions send nothing.

`DASHBOARD_NOTIFIER_SOURCE_STALE_WARNING_SECONDS` accepts integer seconds
1..86400, default 300; Compose passes it to both notifiers. Display-name setting
`DASHBOARD_NOTIFIER_SOURCE_NAME` defaults to Watchdog; Compose sets Kraken on
kraken-notifier. General sink remains TEST_FILE; Kraken keeps its existing sink.
Own operational WARN bypasses the producer-alert severity filter/cooldown and uses
existing yellow/normal and green/silent priority policy in the normal Telegram chat.
No changes to Telegram routing/formatting implementation or the healthcheck:
notifier status remains failed/unhealthy while its source is failed, even after
successful delivery of its own source warning.

`internal/notifier/source_health.go` owns a separate, atomically replaced and
file-synced source-health.json in the notifier store, preserving incident sequence,
first failure time and pending opening/recovery before delivery. Existing durable
decisions and sink receipts deduplicate confirmed notifications after restart,
including receipt success followed by failed decision persistence. Telegram's
pre-receipt remote-acceptance crash window still exists. While the source is bad,
its lifecycle is never replayed or bootstrapped; normal bootstrap/replay resumes
only after healthy evidence returns. Retain source-health.json with its store.
The shared step43-v1 contract is consumed independently of task 1's implementation:
lastSuccessAt describes monitor publication, not a successful Kraken API response;
lastError describes internal source failure. No monitor journal identity changes.

WSL / existing golang:1.23-alpine with --network none: go vet ./... and full
go test -count=1 ./... PASS. New tests cover the 299/300-second boundary, all four
failure categories, changes of cause without resetting the incident, short failure
reset, restart during the delay and after warning, recovery, new incident identity,
failed sends/pending recovery, safe reasons/configuration, and preserved source
bootstrap with no old alerts sent while unhealthy. Telegram HTTP loopback verifies
normal-chat routing with a critical group configured, yellow/audible WARN,
green/silent recovery and receipt deduplication after losing the decision append.
Validation output/scripts: storage/notifier_source_validation/. All four Compose
render cases PASS (PAPER/LIVE, default 300 s / override 600 s), preserving sinks,
source names and existing healthchecks. They use only fixture settings and
--env-file /dev/null; no credential files are loaded. WSL whitespace validation
accepts this Windows checkout's CRLF as line endings (cr-at-eol).

No VPS access/change/restart, real Telegram messages, API keys, real orders,
commits, pushes or releases in this task. Python/task-1 files and pre-existing
local changes preserved. Step59 not needed for this notifier-only change.
Pending: complete task 1 and deploy both in a separate explicitly authorized
release, then accept the actual source-failure/recovery notifications on the VPS.

## Market-data day-close guard reads GET /fapi/v1/time (2026-10-15, local)

User request after the first cycle check below. `BinanceMarketDataClient::serverTimeMs()`
reads `GET /fapi/v1/time` (0 when unavailable); `MarketDataIngestor::waitForBinanceDayClose`
waits on it (at most 30 reads, about 1 s apart; unavailable time or no close throws, so the
cycle retries after `retry_after_failure_seconds`), then ranks the universe once. The
`serverTime` of exchangeInfo is no longer read. The PAPER test's Go Binance fixture now
serves `/fapi/v1/time` (the C++ simulator already did); that Docker test was not rerun.
- Real Binance at 07:33 UTC: `/fapi/v1/time` within 0.3 s of the host clock, exchangeInfo
  `serverTime` frozen 808-814 s behind across three reads.
- Full meson build PASS. New binary `--run-once` against real Binance into a scratch
  database: no wait, 50 symbols / 4,939 rows in 2.9 s (then the expected NATS connect error,
  no NATS in the test). Against the C++ Binance simulator: clock 2026-10-14 00:00 -> 30
  waits then "Binance clock did not reach the end of 2026-10-14"; clock 2026-10-15 00:00 ->
  no wait (the run then stops at the simulator's missing 24h ticker, unrelated).
- `market_data_time_handler_audit.sh` and `live_runtime_clock_audit.sh` PASS. Step59 PASS
  with the unchanged fingerprint `94fdf8d8…` (RealTest 25/25, 50 fills, restart/dashboard/pacing).
- Deployment needs the user's authorization (new runtime image, release, activate).

## First 00:00 UTC cycle on release 20261014_201725 (2026-10-15, read-only check)

Checked at 07:00-07:10 UTC without changing anything on the VPS.
- Market-data started 00:00:00.152 (100 ms delay works), but the new `serverTime` guard
  waited until 00:01:02.955: 27 warnings "Binance clock is ~1798005 ms before the end of
  2026-10-14". The value stayed constant, so `exchangeInfo.serverTime` is a cached document
  (about 30 min old), not Binance's clock. It passed on about attempt 28 of
  `MAX_SERVER_CLOCK_WAITS = 30`; a slower cache refresh would throw and retry the cycle
  300 s later. Once allowed, the download took 2.9 s (97 requests, 16 workers; was 15.8 s).
  Orders reached the simulated exchange at 00:01:12.5 (00:00:28 the day before).
  Fixed locally the same day (next section); not deployed.
- Cycle: 3 active signals, 2 decisions, SELL AVAXUSDT and SELL ONDOUSDT filled at the open
  (net PnL -775.59 and -1,213.33 USD), cash 83,017.75 USD, 3 positions. 23 fills, 23 distinct
  ids; ledger positions equal `simulated_exchange_positions`; 0 open orders. Duplicate
  2026-10-13 updates/snapshots at 00:00:00 were ignored by strategy/risk as before.
- `trading_reconciliation_daily`: 20261014 CLEAN 6 checks (last 23:50), 20261015 CLEAN 7
  checks (hourly 01:00-07:00). `trading_equity_daily` has 20261014 (91,699.43 USD), so the
  Overview Close loop has data for that day (CLEAN); `/api/overview` served 200 with no
  close-loop warning. Visual confirmation of the green day is left to the user.
- PAPER comparison: 6 rows, zero signal differences; 2026-10-15 equity 91,693.999 vs
  expected 91,694.050 USD. Collector refreshes every minute.
- From 06:59:55 UTC every Kraken read failed (API 503, Kraken scheduled "Website and API
  Maintenance", then cancel-only/post-only verification). The monitor keeps sweeping,
  `lastError` "Account observation unavailable or invalid"; `kraken-notifier` is
  unhealthy because its heartbeat source has no recent success. No Telegram message tells
  the user this; the monitor also does not log why a read failed. External cause.
- 09:00 UTC daily Kraken Telegram summary: not yet due at the time of the check.
- Last successful account read (06:59:55): `currencies.BTC.collateral` 1,792.1116 =
  `value` 1,810.2137 x (1 - 0.0099999796), i.e. Kraken's post-haircut USD value with a
  1% haircut; `marginEquity` 1,794.2911 = BTC collateral + USD 2.1795. Wallet currency
  fields are `quantity`, `available`, `value`, `collateral`. The monitor does not read
  `account-log`, so history `info` words and `old_balance`/`new_balance` stay UNCONFIRMED;
  no positions/fills yet, so the fee currency too. USD_RESERVE warning active (2.18 USD).

Independent concurrent-session corroboration at 07:05-07:08 UTC: read-only SQL and
logs agree with the cycle, equity, reconciliation and latest comparison figures
above. Cycle completion logged at 00:01:14.173; fills 22/23 fully filled, no pending
plans or active simulated orders. Five retained Telegram receipts include the
2026-10-14 startup daily summary and three setup tests; no 2026-10-15 summary yet.
Deployed source archive hashes match local OverviewPage, CloseLoopTimeline,
real_overview.go, kraken_account.py and ingestion.cpp; served JS contains the Close
loop panel and green-state rendering. This session did not read login credentials
or exercise authenticated API/browser rendering. At 07:07:33 the monitor still
reported failed observations, last success 06:59:55.144953; 13/14 containers healthy,
all five host services active. Its generic stored error alone does not identify
the upstream cause; the maintenance attribution above belongs to the concurrent
session's separate evidence. No remote changes, restarts, new API reads or orders;
no secret files opened. Existing documentation was being edited concurrently and
was preserved. Documentation-only update: no build/Step59/browser campaign needed.

## Full VPS release and /algoTrading relocation accepted (2026-10-14)

User explicitly authorized deployment of all new work, superseding earlier local
deferrals. Final release: 20261014_201725-d11699d-dirty, source + three runtime/API/web
images and base images with manifest IDs/checksums verified on destination.
All eight application folders moved from / to /algoTrading, keeping configuration,
credentials, trading/message state, logs, reports, backups and earlier releases.
Active comparison study/manifest and Kraken report pointers were rebased; original
documents retained in a protected backup. Historical archives were not rewritten.

Before stopping the old stack, archived its actual running images (including the
mobile dashboard) as pre-root-20261014_201026. Its old source host templates were
adapted only for /algoTrading so rollback can operate at the new root. It remains
releases/previous; backup/restore files and image archives retained. Rollback was
prepared and checksummed, not exercised against running trading state.

WSL validation: full Go suite PASS; notifier/command tests (including routing and
test severity) PASS; installation-root regression 1/1, comparison 14/14, logging
8/8 PASS; all nine C++ runtime executables and shared library rebuilt. Bundle has
38 libraries, runtime ldd resolves all dependencies, procps supplies pgrep for new
process checks. Step59 PASS with accepted fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25, deterministic/restart/dashboard/pacing parity.
A competing local build was observed; stopped our duplicate Ninja process and
packaged only after the shared C++ build completed. Final independent image builds
and exported image IDs were verified.

During relocation, restrictive umask made the new root 0700 and blocked the syslog
user. Corrected root to 0755; installer now explicitly protects public traversal
and template/helper readability while secrets remain 0700/0600. Regression test
covers umask 077. Rsyslog's omprog needed permission to enumerate its own inherited
descriptors; narrowly allowed owner /proc/[0-9]*/fd/ read in AppArmor. Dashboard
access was restored. Kraken notifier inherited the API healthcheck; now shares a
notifier-specific status check respecting its explicit store directory.

Final verification 20:22:42 / 20:24:01 UTC: all 14 containers healthy, zero container
restarts, all bind mounts under /algoTrading, all five host services active. HTTP
anonymous API 401, viewer login 200, Overview/Positions/Reconciliation/Live vs Expected
200. Tailnet HTTPS certificate/HTTP 200 checked with explicit node-IP resolution;
VPS does not resolve MagicDNS locally, existing phone/Tailscale setup unchanged.
Kraken lastSuccessAt 20:22:41 UTC, empty lastError; MODE=paper, no private gateway.
Daily reconciliation history is CLEAN, 0 issues, 2 checks on 20261014.
Five retained comparison rows are OBSERVING with zero signal differences.
No credential matches in current service logs; no file exceeds 50 MiB; logs fresh
at 20:24:00 UTC. Actual UTC rollover/multi-day observation remains separate.

SQL economic hashes/counts, excluding write metadata, match before migration and
after both restarts: 21 fills, 5 open positions, 6 closed positions, runtime state,
simulated cash/positions, funding and venue-cost tables all unchanged. No real orders
or Manual Control trades submitted. Critical-only Telegram routing and colored/silent
policy deployed; INFO and CRITICAL setup messages accepted, 5 receipts total (3 tests).
Kraken uses TELEGRAM; the general watchdog notifier preserves its TEST_FILE sink.

Evidence: /algoTrading/reports/{root-migration,deploy-verification,final-release-evidence}.json,
protected SQL-hash/config/comparison backups in /algoTrading/backups, release manifests
in /algoTrading/releases. Local orchestration/logs/artifacts in ignored
storage/root_migration/. First scheduled daily Telegram summary, hourly reconciliation,
multi-day PAPER/shadow and real-money lifecycle acceptance remain pending.

## Critical-only Telegram destination prepared (2026-10-14, local only)

User created algoTrading CRITICAL. Exact-title lookup using the existing bot's
getUpdates and getChatMember identified one group and confirmed member status;
no offset was advanced, credentials/other messages were not printed. Identifier
evidence is ignored storage/telegram_priority/critical_group.json.
Optional DASHBOARD_TELEGRAM_CRITICAL_CHAT_ID (including command _FILE support)
routes CRITICAL events and their recoveries to that group. WARN/INFO/unknown
events stay in the primary chat; unset option preserves single-chat behavior.
Both chain notifier services pass the optional setting through Compose.
Resolved critical alerts remain silent and green in the critical group.
Prior receipted events are not replayed on a destination change. No VPS
configuration/image rollout or real message sends for this preparation.
WSL Go tests pass for internal/notifier and cmd/dashboard-alert-notifier in the
existing golang:1.23-alpine container with --network none. Loopback payload tests
cover critical/recovery routing, other severities, normalized labels, optional
configuration, original single-chat fallback and receipt dedup across routing
changes; existing redaction and retry tests remain passing.

## Telegram priority markers and sound policy (2026-10-14, local only)

Shared notifier adds red CRITICAL/URGENT, yellow WARN, blue INFO and green resolved
markers. INFO/daily portfolios and resolved alerts use Bot API
disable_notification=true; WARN/CRITICAL request normal notifications, subject
to user chat/phone settings. Unknown severity stays audible by default. No custom
tone is selected by the bot; Telegram configures tones per chat.

WSL validation with existing golang:1.23-alpine image and --network none:
go test ./internal/notifier ./cmd/dashboard-alert-notifier PASS. Six loopback HTTP
cases verify actual color/text and silence payload for critical, warning, info,
critical recovery, normalized labels and unknown severity; existing durable receipt
deduplication, bounded plain messages, rate limits and secret-redaction tests pass.
No real Telegram sends or VPS/container changes for this work; rollout pending.

## Kraken read-only monitor and Telegram activated on VPS (2026-10-14)

User explicitly authorized this activation only. Existing locally configured
Kraken and Telegram credentials were consumed internally and transferred over
SSH without printing their values. Kraken accepted a permission-checked account
snapshot: General READ_ONLY and transfer NO_ACCESS. The dedicated VPS key file
/secrets/kraken_readonly.env is root-owned mode 0600. Existing chain credentials
were preserved; their pre-change backup remains protected in /secrets.

At the current installation root /, enabled algotrading-kraken.service with
30-second reads, raw sample retention 3 days and 09:00 UTC daily summaries.
Set COMPOSE_PROFILES=kraken, KRAKEN_NOTIFIER_SINK=TELEGRAM and added only
kraken-notifier via algo up --no-deps. The first observation and explicit Telegram
setup message passed. Verification at 18:56:58 UTC: monitor active/enabled,
lastSuccessAt 18:56:49.760128 UTC, empty lastError and three Telegram delivery
receipts. All 13 original container IDs remained unchanged; MODE stays paper,
gateway-kraken is absent and no private orders were submitted.

Evidence on VPS: /reports/kraken-monitor-activation.json and container identity
snapshots /reports/kraken-activation-containers-{before,after}.txt. Protected
credential/config backups retained; local orchestration in ignored
storage/kraken_activation/. First scheduled daily summary and restart deduplication
acceptance are still pending. This activation does not deploy other local work,
migrate the root to /algoTrading or confirm Kraken trading/account-history fields.

## Installation root /algoTrading (2026-10-14, local only)

The user changed the requested VPS root from / to /algoTrading, then explicitly
cancelled deployment. No VPS migration or remote writes were started in this task.
algo install now renders installed systemd, rsyslog and AppArmor host paths for
the selected root. Compose container destinations and OS-owned paths stay fixed.
All eight application folders live under that root; existing configuration,
credentials and data are preserved on reinstallation. Operating examples now
use /algoTrading; the running VPS still uses /.

WSL validation: chain_installation_root_test.py passes (1 test), covering host
paths, fixed container/OS paths, preserved configuration/state and idempotent
reinstall; comparison tests pass (14), daily log tests pass (8). Runtime bundle
(9 executables, 38 libraries), runtime/API/web Docker builds and local release
export succeeded. No images or release were activated on the VPS.
Canonical Step59 passes: 107 days, 50 fills, RealTest 25/25, no differences;
determinism, system/dashboard restart and pacing checks retain fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Future relocation needs state preservation, replacement of installed host
units/AppArmor and rebasing active comparison study/manifest host paths;
installing at another root alone does not migrate state.

## Superseded PAPER deployment retired (2026-10-14, local only)

User explicitly requested step 3 after the local baseline fix, without deployment.
Retired all ten tracked deploy/paper_trading files: old launcher, three systemd
units, activity/VPS overlays, old guide and LAN phone bridge. The useful Compose
topology and logging overlay moved to validation/fixtures/paper (two of those ten
files preserved as fixtures), keeping recovery/shadow service and volume contracts.
Retired the bridge-specific mobile_access_test; private Tailscale/mobile layout
remain supported. Ignored old bundles, credential files and accepted run evidence
were not deleted; the physical old directory may still hold ignored artifacts.

--paper bundling and chain/algo builds now use deploy/chain/.runtime_bundle.
PAPER/shadow tests default to current chain image tags and resolve build contexts
from their fixture directory. Repository collector defaults use chain service names
and ignored storage/chain; installed runtime defaults are unchanged. Maintained
guides/Telegram instructions now point to chain or isolated validation. Historical
migration evidence and frozen venue records are preserved.

Validation: syntax checks, 14 comparison tests, 3 monitor tests (one
opt-in Docker test skipped), 8 logging tests and 5 shadow unit tests PASS. Full
isolated shadow campaign PASS in
storage/paper_validation/algotrading-paper-2a37250ac5ae/: actual producer plans,
read-only fixture, skips, faults and durable/restarted observations. PAPER/recovery PASS in
storage/paper_validation/algotrading-paper-a870571ff893/: completed-bar/fill/
dashboard/restart checks, HTTP/NATS/PostgreSQL faults, duplicate handling,
maintenance backup and fresh-volume restore with identical SQL tables/files.
The new-path PAPER bundle
build passes (9 executables, 38 shared libraries); runtime and API validation images
were built successfully. This cleanup changes no C++ source. Step59
PASS with the accepted fingerprint, 25/25 RealTest, zero differences and restart/
dashboard/pacing equality. The moved 15-service Compose renders identically to the
original apart from current image tags/bundle location; logging overlay renders.
Meson compile hit the existing Docker-bind-mount path mismatch; the redundant
Ninja rebuild was stopped and accepted unchanged C++ artifacts were used.
Source-reference/whitespace checks and 4 AI index tests pass; navigation refreshed.
No VPS connection, installation, restart,
private orders or Telegram messages were performed for this source cleanup.

## Baseline strategy seed corrected (2026-10-14, local only)

User authorized step 1 without deployment. comparison.py now persists the strategy
intent alongside PAPER/LIVE seeds and uses its signals, retaining cash, positions,
desired weights and virtual positions from the execution snapshot. Date and strategy
IDs must match; missing signal payloads are rejected. An older seed is recovered from
the exact historical PostgreSQL checkpoint, never today's intent or held positions.
The original study/manifest/fast/comparison files are preserved once under
before-seed-repair/. Missing historical evidence leaves the study unavailable.

Fast cache identity now hashes baseline economic inputs and profile/portfolio bytes,
excluding observational cycle state; seed/input changes rerun the baseline even at
the same cycle count. The identity is recorded only after a successful matching
output, so failed recalculation remains retryable. Configuration-change archives
include the cache identity.

WSL paper_comparison_test.py: 14/14 PASS, including the 40..50 RSI hold-vs-close
economic regression, historical repair/preservation/restart, absent/wrong-date/wrong-ID
checkpoints, seeded LIVE with empty execution signals, configuration-switch date
rejection, and cache invalidation/failure retry. Canonical Step59 PASS: RealTest
25/25, zero differences, 50 fills, rerun/restart/dashboard/pacing equality; fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2` unchanged.
WSL diff whitespace check passes; generated .ai navigation refreshed.
No VPS connection, deployment, collector restart or original study
recalculation was performed in this implementation turn.

## PAPER baseline seed mismatch explained (2026-10-14, isolated reproduction)

The four first-volatility-cycle signal differences come from baseline initialization.
At the portfolio configuration switch, comparison.py copied the execution snapshot
into the seed. Its strategy `signals` and `desired_weights` are empty (execution
owns fills/virtual positions, while strategy checkpoints own signal state). The
strategy checkpoint at seed decision 20261012 has AVAX, NEAR, ONDO, QNT and WLD
active. paper_baseline.cpp restored the empty execution `signals` verbatim.

At decision close 20261013, RSI(7) on the frozen 100-day window is AVAX 42.81665,
NEAR 49.96914, ONDO 41.73249, WLD 47.23346. PAPER correctly retains these active
signals (exit below 40); the unseeded baseline cannot open them (entry above 50)
and flattens the copied holdings. QNT is 38.48881 (exit), PEPE 53.12285 (entry).
The smaller expected active set also changes volatility sizing for PEPE.

Controlled replay uses the exact deployed runtime image, frozen market/profile/
portfolio, cash, positions and opening prices, without network or service changes.
Unchanged seed reproduces the retained fast.json exactly. Changing ONLY the seed
signals to the persisted strategy intent removes all four signal differences:
expected PEPE 4170812.73393 -> 711394.39138 units (PAPER 711394.39138).
Expected equity 92170.128671916 -> 92196.61532922616 USD; PAPER is
92196.61655036971 (remaining gap 0.00122114355 USD). Expected cash after correction
77254.13183556445 versus PAPER 77252.91191313534; ONDO/WLD quantity differences
match the documented close-vs-next-open sizing convention.

Evidence: storage/paper_cleanup/reproduction-result.json (control equality and
per-asset results), rsi-result.json and the reproduction scripts; remote isolated
artifacts under /tmp/algotrading-baseline-audit-_ceokz8z. No running collector,
original study/fast outputs, trading account or deployment was modified. Required
fix: seed from the strategy checkpoint at the SAME decision date, not the execution
snapshot; apply also to LIVE seeds; invalidate cached baseline on seed changes.
The local implementation is now recorded above; deployment/recalculation and
multi-day acceptance remain pending.

## Superseded PAPER deployment cleanup prerequisite (2026-10-14)

User requested removing superseded deploy/paper_trading files after chain acceptance.
Read-only VPS check: chain services up; dashboard/database healthy; host receiver and
monitor running; comparison fresh. The first volatility-target cycle (decision
20261013, execution 20261014) reports four signal differences: AVAXUSDT, NEARUSDT,
ONDOUSDT and WLDUSDT are 1 in PAPER and absent in fast expected. Equity is
92196.61655036971 versus 92170.128671916 USD. The cause is reproduced above;
the retained running comparison has not yet been corrected.
Multi-day/economic acceptance remains pending; no tracked files removed yet.
The old Compose still feeds paper_trading_test and therefore recovery/shadow;
algo/build_runtime_bundle still use its ignored bundle directory. Cleanup must
migrate these dependencies instead of deleting useful validation. Review in
storage/paper_cleanup/REVIEW.md; awaiting the user's choice to investigate first or retire
the old deployment independently of pending economic acceptance.

## Mobile dashboard layout deployed (2026-10-14)

User requested a bounded mobile adaptation. Below 900px the dashboard removes
the desktop body minimum width, uses a collapsible navigation drawer and stacks
panels. Narrow phones use one-column metrics; wide tables scroll inside their
panels. Headers, date controls, inspectors and login fit the viewport; touch can
scroll vertically over charts. Desktop styles remain outside these breakpoints.

- WSL Node/TypeScript/Vite builds pass for mock and API bundles.
- `validation/mobile_dashboard_browser_test.py`: Chromium PASS, 44 page/viewport
  checks (11 pages at 360/390/768/1440px), no document overflow or page errors;
  navigation selection/Escape/backdrop, date picker and mobile login also pass.
  Mock screenshots/results: `storage/mobile_dashboard/browser/`.
- Built from the VPS release's frontend plus only AppShell/mobile CSS changes,
  excluding unrelated current local work. Deployed image
  `algotrading-dash-web:mobile-20261014` under the existing chain tag; evidence,
  source and image archive in `/releases/dashboard-mobile-20261014/`.
  Archive SHA-256: `51d8ccbdbd1f4c9553043feadf452e0291bf80917c0bcd39553ec2d360f75d4b`.
- VPS: dash-web healthy; valid Tailscale HTTPS frontend/assets, unauthenticated
  API 401; before/after container IDs confirm only dash-web was recreated.
  Previous image retained as `algotrading-dash-web:before-mobile-20261014`.
  Rollback: retag it to `algotrading-dash-web:chain`, then
  `sudo /chain/algo up --no-deps dash-web`.
- User accepted the mobile result in chat after deployment. No trading/core changes;
  canonical Step59 was not rerun for this presentation-only update.

## Research on CURRENT lib: confirmed done (2026-10-14)

Checked for the user's step 6/7 list: `research/src/legacy/runtime/` was removed in commit
42b2ff0 (80 files, about 19,700 lines) and every research executable in
`research/src/{legacy,canonical,tools}` links `libalgolib`; they build in every full build
and the fast baseline/step59 use them. Only AGENTS.md "Current direction" still listed the
migration and removal as pending; corrected.

Then (user request): `research/src/legacy/` renamed to `research/src/backtests/` (meson
`subdir('backtests')`, `backtests_inc`; binaries now `build/research/src/backtests/`). Paths
updated in comparison.py, build_runtime_bundle.sh, replay.py, REPLAY.md, ARCHITECTURE_FULL,
CONTEXT_FULL, AGENTS.md and the validation scripts (paper_comparison_test,
research_html_report_gate, research_scenario_gate, step58a_canonical_replay_suite_gate).
Historical CURRENT_STATE text and the `.ai` index test's example paths keep the old name.

Step 8: the `tools/historical_replay` step58/step58a runners were retired. Their build
scripts compiled a hand-listed subset of `lib/` sources and no longer linked (both failed
with `ld` errors on 2026-10-14); nothing used them outside their own run scripts, and
`research/replay.py` (CURRENT `ReplayRuntime`) already writes the dashboard simulation
state they used to write. Removed: the two runner sources, two build scripts, three run
scripts, two step58a check scripts and `config/historical_replay/step58_dashboard_simulation_v1.json`
(only referenced by frozen checksums). They stay in Git history; `docs/venue/step58*/SHA256SUMS`
still lists their recorded hashes as evidence.

Checks after the rename and removal: full meson build (7 research executables in
`build/research/src/backtests/`, stale `build/research/src/legacy` deleted); PAPER runtime
bundle builds with its 9 executables; paper_comparison_test 10/10; ai_index_test 4/4;
step59 PASS with the unchanged fingerprint `94fdf8d8…` (RealTest 25/25). (The two Python
tests first failed inside the combined script with an import-path FileNotFoundError and
passed when rerun on their own.)

## Kraken coverage refreshed; one year of the strategy universe (2026-10-14)

ROADMAP 21. Public data only, nothing deployed.
- `tools/exchange_coverage.py` at 15:55 UTC, window 2026-09-19..2026-10-13 (25 completed days),
  523 Binance USDT perpetuals: Kraken 39/50 (96.19% of top-50 turnover), Hyperliquid main
  38/50 (94.49%), with HIP-3 39/50 (95.92%). Kraken lacks ranks 24-50 only: 龙虾, AKE, BTW,
  RLC, ONE, US, BR, MAGIC, GTC, LIT, NIL; all of today's top 20 (the strategy universe) are
  on Kraken. Evidence: storage/exchange_coverage/20261014T155526Z/.
- `tools/kraken_symbol_map.py` rebuilt: 161 mapped, identical to the committed table except
  `generated_at`; the repository file was left unchanged.
- New `tools/universe_coverage_history.py` (+ `validation/universe_coverage_history_test.py`,
  2 offline tests): daily top-20 by the 25-day mean quote turnover over 2025-10-04..2026-10-13
  (375 days) against today's Kraken map and Hyperliquid listing. Kraken: a missing asset on
  368 days, 3.83 per day; with RSI(7) > 50 on 266 days (1.29/day), > 80 on 79 days
  (0.24/day); 25 of the last 30 days had one, none on 10-09..10-13. Hyperliquid: 367 days,
  4.24/day, RSI>50 294 days, RSI>80 87 days. Most missed on Kraken: PIPPIN 125 days, LAB 97,
  BEAT 80, AKE 66, SIREN 64. Approximations: today's listings (delisted Binance coins absent,
  later Kraken listings counted as always available); RSI above the entry is a candidate,
  not a guaranteed entry. Evidence: storage/exchange_coverage/universe-history-20261014/.
- Checks: `validation/exchange_coverage_test.py --snapshot .../20261014T155526Z` 5 tests and
  COVERAGE-AUDIT PASS (all candles, ranking, filters, aliases, three counts); the new tool's
  2 offline tests; the tool reproduced the exploratory script's numbers exactly.

## Container health checks and the real close loop (2026-10-14, local)

User request after the deployment: Infrastructure showed nats and the 7 trading
containers as "Degraded" (they had no Docker health check; `monitor.py` reports a running
container without one as WARN) and Overview had no Close loop panel in PAPER/LIVE (the real
provider never filled `closeLoop`, only the replay did; not a regression).
- `deploy/chain/compose.yml`: `nats` checks `wget http://127.0.0.1:8222/healthz` (server
  and JetStream ready); market-data, strategy, risk, planner, execution, gateway,
  sim-exchange and gateway-kraken check `pgrep -f '^<executable>( |$)'` (anchor `x-process-check`,
  30 s, 2 retries, 20 s start period). That means "alive", not "trading correctly": the
  services have no in-loop heartbeat. Startup order (`depends_on`) unchanged.
- Close loop: ExecutionState records every reconciliation against the exchange snapshot in
  the new `trading_reconciliation_daily` (date of the snapshot, CLEAN/BLOCKED, issues,
  checks); a day stays BLOCKED once any check found issues. Not a column of
  `trading_equity_daily` because that row is written only once reconciled, so it would
  always say clean. A failed write is logged and never stops trading. dash-api reads it
  (`CloseLoop`, empty before the table exists) and sets `closeLoop` per equity day; days
  before the history began stay without it (not assumed clean), so the panel appears from
  the first equity day after deployment.
- Checks: both health checks tried in throwaway containers (own name matches under
  docker-init, other names and prefixes do not; NATS /healthz OK); compose config for
  paper and live; upsert/read SQL on PostgreSQL 16 (CLEAN twice -> CLEAN 2 checks; CLEAN,
  BLOCKED, CLEAN -> BLOCKED); Go vet + tests (new close loop case in
  `runtime_accounting_test.go`); full meson build; 15/15 component tests; step59 PASS with
  the unchanged fingerprint `94fdf8d8…` (RealTest 25/25). Deployed inside the full release
  `20261014_201725-d11699d-dirty` (see "Full VPS release and /algoTrading relocation" above).
  Checked on the VPS at 20:47 UTC: every container (healthy) with the new checks,
  `trading_reconciliation_daily` written (2026-10-14 CLEAN, 2 checks), logs flowing under
  /algoTrading/logs; the local tree differs from that release only in docs and a comment
  indentation in rsyslog.apparmor, so no further deployment was needed. A first release attempt
  (`20261014_194814-d11699d-dirty`) was deleted unused: a Docker credential-helper error
  stopped the runtime image build and the release would have packed the old runtime image.
  `pgrep` is present in plain ubuntu:24.04, so the process checks do not depend on the
  `procps` line added to `deploy/live/runtime.Dockerfile`.

## VPS deployment of the 2026-10-14 local work (2026-10-14)

Authorized by the user. Release `20261014_175134-6bdac56-dirty` (working tree on 6bdac56,
includes the mobile dashboard layout deployed earlier as `dashboard-mobile-20261014`):
SHA256SUMS verified on the VPS. Before activating: `/config/market_data.json` backed up as
`market_data.json.before-20261014` and changed to `midnight_delay_ms: 100`,
`max_parallel_requests: 16`; `/releases/current` set to `20261013_212613-94df131-dirty`
so `algo rollback` returns to it (that release has the pre-mobile dash-web). `activate` at
18:26 UTC: backup, install, `:chain` retagged, `data/STATE_FORMAT` = 1, 13 containers up.
Updated host units `algotrading-comparison` and `algotrading-kraken` copied +
daemon-reload; comparison restarted ("PAPER comparison refreshed"); `algotrading-kraken`
stays inactive (no read-only key on the VPS yet).
Verified: no error lines in any service, risk restored its 2026-10-13 checkpoint,
reconciliation clean (cash 77,252.91 USD, 5 positions, 0 open orders), equity history
backfilled, dashboard 200 / API 401 unauthenticated, Tailscale Serve still proxies it.
Not done: `DASHBOARD_OPERATOR_PASSWORD` in `/secrets/chain.env` (user's credential; no
operator login until added). Pending: check the first 00:00 UTC cycle with the faster
market-data settings.

## VPS mobile dashboard access via Tailscale (2026-10-14)

User authorized installing private mobile access on the VPS. Installed Tailscale
1.104.1 from its official signed Ubuntu noble repository; `tailscaled` enabled
for boot. Existing dashboard still listens on `127.0.0.1:8092`; no trading
container restart was required. Started account enrollment with hostname
`algotrading`, `--accept-dns=false` and `--accept-routes=false`.
Account enrollment completed; VPS and Android phone belong to the same tailnet.
Persistent HTTPS Serve proxies `https://algotrading.taild27a89.ts.net/` to the
existing loopback dashboard. Serve status confirms tailnet-only access and no
Funnel configuration. Verified valid TLS with curl (no insecure bypass), HTTP/2
200 for the frontend and 401 for unauthenticated `/api/overview`; phone ping
succeeded through the Madrid relay. `tailscaled` is enabled and active.
User accepted mobile access/layout in chat; exhaustive phone-specific API/chart
interaction was not separately tested. Review tailnet access policy
if other users/devices are added. Mobile access is requested again and supersedes
the earlier cancellation for this scope. See deploy/chain/README.md.

## Live vs Expected for Kraken LIVE (2026-10-14, local)

ROADMAP 13. Changed locally, not deployed; PAPER comparison unchanged:
- `deploy/chain/comparison.py` with `MODE=live` (`collect_live`): reads the Kraken account
  snapshot, real fills, `trading_venue_costs` and `trading_daily_collateral`; starts the study
  at the first observed cycle (seed) and compares from the next; fetches the Binance open of
  each execution day for every asset the account or expected run may trade (kept in
  study.json); gives the fast baseline that day's collateral and the USD capital flows since
  the previous cycle; the latest row (and the seed) follows the account until the next cycle
  because Kraken fees/funding arrive after the fills; output `data/live/comparison/`.
  Shared helpers `check_profiles`, `freeze_market`, `run_baseline` (PAPER path identical).
- Rows add `liveFees`, `expectedFees` (`LIVE_COMMISSION_RATE`, default 0.0005, new in
  chain.env), `liveFunding`, `liveInterest`, `capitalFlows`, `slippage` (fills of the
  cycle's planned orders vs the Binance open; Manual Control fills excluded) and `skipped`
  (MARKET_UNAVAILABLE / MARKET_NOT_TRADING / BELOW_MINIMUM_QUANTITY). The dashboard page
  shows "Where the difference comes from" (each part's effect on equity, rest = rounding,
  sizing, skipped markets) and extra daily columns, only when the fields exist.
- `TradingEngine::onBarClose(..., extraCapital = 0)`; `paper_baseline` takes per-cycle
  `collateral_usd` (extra sizing capital) and `capital_flow_usd`, and reports cumulative `fees`.
- `algo` no longer clears `PAPER_COMPARISON_FILE` in live mode.
- Checks: full meson build; `validation/paper_comparison_test.py` 10/10 (new: baseline with
  collateral 100000 → 50 units and fee 20, a 1000 USD flow → 25.25 units; difference parts
  since the start with a manual fill excluded from slippage and a pre-start fee ignored;
  Binance open of the exact day, another day's candle refused; collect_live seed → first
  cycle with collateral 613.8, flow 50, skipped PEPEUSDT, start cash 900, rate 0.0005 → a
  late fee updates the same row); TypeScript `tsc -b` clean; Go vet + all tests; 15/15
  component tests; step59 PASS with the unchanged fingerprint `94fdf8d8…` (RealTest 25/25).

## Kraken BTC/USD collateral, capital flows, liquidation distance (2026-10-14, local)

ROADMAP 8. User decisions: the strategy sizes with USD + BTC after Kraken's haircut; USD
deposits, withdrawals and conversions are capital flows (not PnL); interest is a cost.
Changed locally, not deployed:
- Sizing capital: the gateway snapshot carries `collateral_usd` = Σ `collateral` (Kraken's
  post-haircut USD value) of the non-USD wallet currencies; reconciliation ignores it.
  Execution-state keeps the latest value, fixes it per day in `trading_daily_collateral`
  (first use wins, so a redelivered daily snapshot is byte-identical for risk) and puts it
  in the daily `AccountSnapshot`; `PortfolioRiskEngine::accountEquity` adds it; Manual
  Control (execution-state and the dashboard preview) uses the same capital. Orders never
  spend it (no cash check exists; cash may go negative as before). Field written only when
  non-zero: PAPER/replay messages unchanged.
- Account history: besides fees/funding, USD changes of the `flex` wallet
  (`new_balance - old_balance`) with "interest" in `info` become INTEREST and with
  "transfer"/"conversion" CAPITAL_FLOW (`VenueCostKind`, one enum for gateway, journal,
  wire and store). BTC/other-wallet entries are logged once as collateral-only; unknown USD
  kinds stay "not booked" (reconciliation then pauses). A deposit no longer pauses LIVE.
- `trading_equity_daily` gains `capital_flows`, `interest`, `collateral_usd` (ALTER ... ADD
  COLUMN IF NOT EXISTS; equity stays cash + positions); the equity backfill replays interest
  and flows. Dashboard reads the table with `SELECT *` so an older database still works.
- Reporting (warning-only, no new thresholds): `kraken_portfolio_summary.collateral_risk`
  gives BTC quantity/value/collateral/haircut, maintenance headroom and the price drop that
  would reach maintenance margin (BTC alone; BTC plus net long positions, linear estimate).
  Daily Telegram portfolio adds three lines; the monitor writes `account.json`; dash-api
  mounts it (`DASHBOARD_KRAKEN_ACCOUNT_FILE`) and the LIVE Overview adds BTC Collateral,
  Sizing Capital, Margin Headroom, Drop to Maintenance and Net Deposits (stale after 5 min).
- Warning (user request after the step): MAINTENANCE_DROP, WARN, when a fall of less than
  25% of BTC and the net long positions would reach maintenance margin
  (`--maintenance-drop-warning 0.25`, explicit in `algotrading-kraken.service`); sent once,
  resolved when it clears, unavailable (no false recovery) without fresh marks, never with
  only short exposure. No stop or order. Monitor tests 15 (new boundary test: exactly 25%
  silent, 24.7% warns, missing prices keep it, recovery resolves); other monitor tests'
  samples now carry no BTC so they keep testing their own warning.
- UNCONFIRMED until the read-only key: history `info` words, `old_balance`/`new_balance`
  semantics, the funded `collateral` value.
- Checks: full meson build; venue adapter test 103/103 (snapshot collateral 613.8; new
  test with deposit, withdrawal, conversion, interest, BTC and other-wallet entries, an
  unknown kind and restart); new store SQL against PostgreSQL 16 starting from the older
  `trading_equity_daily` (ALTER keeps the row, collateral fixed at first use, interest/flows
  only, new columns written); portfolio risk diagnostics (collateral 500 → reference capital
  1480, negative refused); manual_chain_test (collateral and CAPITAL_FLOW round trips,
  unchanged bytes without them); 15/15 component tests; Python account 26, monitor 14,
  portfolio summary 6 (new: collateral lines, `account.json`, 20.0%/22.8% drops, unknown
  without marks, no long exposure); Go vet + all tests (new `TestKrakenCollateralCardsOnlyInLive`,
  `TestDropLabel`); step59 PASS with the unchanged fingerprint `94fdf8d8…` (RealTest 25/25).

## Releases with rollback (2026-10-14, local)

ROADMAP 15. `deploy/chain/algo` (runbook: deploy/chain/README.md "Releases"), not deployed:
- `algo release` saves the chain images as `<name>:<version>` (loading no longer replaces the
  running `:chain` images), writes `release.json` (commit, image IDs, architecture,
  `state_format`) and a copy of `algo`; `SHA256SUMS` covers all four files.
- `algo activate <folder> [root]` (also from the release's own `algo`): refuses 23:30-00:30
  UTC, a checksum mismatch, an image ID or architecture mismatch, `chain.env` images other
  than `:chain`, a folder outside `releases/`, and an older state format than `data/STATE_FORMAT`;
  then down, `algo backup`, extract source, `algo install`, move `:chain`, record
  `releases/current|previous|history.log`, up; reports host units that differ (not replaced).
  Images are loaded only if missing. `algo rollback` activates `previous`; `algo releases` lists.
- Older releases without `release.json` are accepted (SHA256SUMS only, state format 1, images
  kept as `<name>:<version>`). Before the first VPS activation record the running release:
  `echo 20261013_212613-94df131-dirty | sudo tee /releases/current`. Rolling back to it
  reinstalls its `comparison.py`, without the two fixes applied in place afterwards (see
  "VPS PAPER switched to volatility targeting").
- The dispatcher is `main "$@"; exit` so bash never reads the launcher file that `install`
  replaces during `activate`/`rollback`.
- Local acceptance (scratch installation `/home/juan/reltest`, full chain, since removed):
  two releases with a different dash-web image; activate A from its folder (first activation,
  loaded), activate B from the installed `algo`, rollback to A without reloading, the running
  containers' image IDs matched the manifests each time; refusals of state format 2, a changed
  `release.json` and a folder outside `releases/` left A running; an old-format release (HEAD
  source and launcher, `:chain` tags, no manifest) activated, then forward again from B's own
  `algo`; one backup per activation (5), history log correct. 22/22 functional checks pass;
  the test's own cleanup removed the local `algotrading-dash-web:chain` image, rebuilt from
  the current source.

## Kraken fees and funding; comparable LIVE cash (2026-10-14, local)

ROADMAP 9 (user decision: nothing in PAPER; with Kraken update fees/funding while a
position is open and keep the final values at close). Changed locally, not deployed:
- Gateway (venue mode): Kraken fills carry no fee, so the adapter reads the account history
  (`GET /api/history/v2/account-log?count=500`, signed with the query as postData; path from
  the python-kraken-sdk, the docs site was not readable) every 5 min and 30 s after a new
  fill. `fee` (paid, USD) with `execution` = fill id, and `realized_funding`, become journaled
  `cash_entries` (once per log id; first read of a journal = baseline, not booked; a fee
  listed before its fill waits up to 10 min) and are published as `VenueCostEvent`
  (`execution.event.venue_cost.v1`, new subject merged into the stream). Other balance
  changes (transfers, conversions, interest, non-USD fees) are logged once as not booked.
  A failing history read no longer stops the poll.
- Execution-state: applies each cost once (cash `Account::applyCashMovement`, cost row in
  `trading_venue_costs` and the snapshot in one transaction; funding also into
  `trading_funding`), then rebuilds the ledger with venue fees added to their fill's
  commission; publishes an account snapshot. PAPER produces no costs (unchanged).
- Found on the way (LIVE blocker): execution-state keeps spot-style cash (a buy spends its
  value) but Kraken's USD balance does not move when a perpetual is opened, so the first
  reconciliation after a real fill would have reported CashMismatch and paused trading. The
  venue snapshot now reports USD balance minus Σ position size x `openPositions[].price`;
  reconciliation cash tolerance is `--cash-tolerance-usd` (PAPER 1e-6 unchanged, chain LIVE
  0.50 USD via `./algo`), mirrored by the dashboard Reconciliation page.
- UNCONFIRMED until the read-only key / first authorized order: history fields and info
  values, funding sign, fee currency, `price` = average entry, realized PnL settled into
  the USD balance at once. The read-only probe now also reads the account history.
- Checks: full meson build; venue adapter test 95/95 (new: fees and funding from the
  account history incl. baseline, deferral, unbooked kinds, signing and restart; snapshot
  cash convention); the new store SQL against a temporary PostgreSQL 16 (save with
  snapshot, duplicate refused, fees per fill, funding row); 15/15 component tests; Go tests
  incl. `TestKrakenLiveCashToleranceMatchesExecutionState`; step59 PASS with the unchanged
  fingerprint `94fdf8d8…` (RealTest 25/25).

## Manual Control in the PAPER/LIVE chain (2026-10-14, local)

User decisions: a manual request confirmed during the day executes at once at the current
market price (also in PAPER), and the next 00:00 UTC daily cycle returns to the strategy
targets (as in the replay). Changed locally, not deployed:
- Flow: dash-api (OPERATOR) validates the CSV against the strictest strategy limits of the
  risk checkpoint's portfolio config and the tradable assets (PAPER: top-N + held; LIVE:
  Kraken contracts verified on Symbol Mapping), previews at the latest closes and writes
  `data/manual/requests/<ns>-<id>.request` (`real_manual.go`). Execution-state
  (`--manual-dir --portfolio-config --price-url`) checks again (reconciled, no open order,
  daily cycle finished, not 23:45-00:15 UTC, request < 2 min old, limits), prices every
  touched asset with Binance `/fapi/v1/ticker/price` (< 5 min old), plans each strategy on
  its share of equity with `NotionalOrderPlanner`, writes the processed record before any
  order (so a request never runs twice) and submits through `ExecutionEngine::applyManualOrders`
  (daily markers untouched). Rejections are recorded with their reason; nothing waits.
- PAPER fill: orders carry `fill_at_reference_price`; the simulated exchange fills them at
  once at that price (`SimulatedExchange::fillAtPrice`). LIVE: Kraken at the market through
  `gateway-kraken`; the page shows `LIVE_ORDER_SUBMISSION_DISABLED` unless submission is enabled.
- Re-sync: the next daily `AccountSnapshot` carries `resync_to_targets` (marker
  `data/manual/resync_state`, bound to that day so a redelivery is identical); risk then
  applies `resyncToApprovedTargets` (`lib/src/risk/target_decisions.*`, now shared with the
  replay; it also adds a strategy whose decisions were all HOLD).
- Fix found on the way (LIVE): daily-plan orders reached the gateway without
  `reference_price`, so Kraken venue mode would have rejected every one with
  NO_REFERENCE_PRICE. Execution-state now sets it (close(T)) and persists it with recovered
  orders. New wire fields are written only when set, so other messages keep their bytes.
- Chain: `data/manual/{requests,processed}` (10001, 2770; dash-api writes 0644 files),
  `DASHBOARD_OPERATOR_PASSWORD` (generated for new installs; add it to an existing
  `secrets/chain.env`), `DASHBOARD_LIVE_ORDER_SUBMISSION`. Page wording per mode, LIVE banner.
- Checks: meson build; 15/15 component tests incl. new `manual_chain_test.cpp`; Go vet/tests
  incl. `real_manual_test.go`; chain images build (web type-check). Local PAPER chain from a
  scratch install (real public Binance data, no keys): manual 10% BTC + 5% ETH went
  QUEUED -> AT_EXCHANGE -> FILLED in ~3 s at the ticker prices (fees 0.1%, cash 84,985); a
  second request closed BTC exactly (no dust) and bought ETH; 160% per asset refused in the
  preview; a hand-written 4-hour-old request rejected; restart: clean reconciliation, nothing
  re-executed; runtime ledger rows correct. The 00:00 re-sync was not observed locally (needs
  a day change): check it on the VPS the night after the first request. step59 PASS with
  the unchanged fingerprint `94fdf8d8…` (RealTest 25/25; `research/replay.py` now also
  compiles `target_decisions.cpp`).

## Faster daily market-data run; Infrastructure range picker in the heading (2026-10-14, local)

First vol-target cycle on the VPS (2026-10-14): market-data started 00:00:05, finished
00:00:20.9 (93 candle requests then 93 opening-price requests, 8 at a time in batches, a new
TLS connection per request, ~0.27 s each); orders reached the simulated exchange at 00:00:28.
Changed locally, not deployed:
- `market_data_service`: start 100 ms after midnight (`midnight_delay_ms`, replaces
  `midnight_delay_seconds`, which is now rejected); `serverTime` guard so only candles closed
  on Binance's clock are stored; a 16-worker pool with one reused curl handle per worker;
  opening prices download alongside the candles; exchangeInfo and 24h tickers in parallel;
  an open with no trade yet is polled 30 s (250 ms) instead of failing to the 300 s retry.
- Real Binance from the local PC (database on WSL ext4): ranking 0.6 s, full 50-symbol x 100-day
  backfill plus opens 3 s, daily incremental plus opens 1.5 s. Build and market-data audits pass.
- `live_sqlite_ownership_audit` now approves the gateway's `venue/venue_journal.cpp` (its own
  `--venue-journal` file) and fails if that file names a canonical market table; it passes, and a
  planted `ohlcv_data` reference makes it fail as intended.
- Deploy note: the VPS `/config/market_data.json` is created once and still has
  `midnight_delay_seconds`; replace it with `"midnight_delay_ms": 100` and set
  `"max_parallel_requests": 16` before starting the new image, or the service refuses to start.
- Reconciler quantity tolerance (`lib/src/recovery/reconciler.*`): positions and open-order
  quantities now match within max(1e-10 units, 1e-12 x the larger quantity). The fixed 1e-10
  was below double rounding noise from ~1 million units (tonight's PAPER PEPE order was 711,394),
  so a false PositionMismatch would have paused trading (ExecutionState stops handing decisions
  to the planner until a clean snapshot). A real difference of one unit still blocks, even at
  1e10 units; cash tolerance unchanged. New `validation/reconciler_quantity_tolerance_test.cpp`
  in `mock_component_tests.sh` (13/13 pass); full meson build and step59 PASS with the
  unchanged fingerprint `94fdf8d8…` (RealTest 25/25). The simulator's 8-decimal quantity grid (backtests
  above ~10 million units) stays as is: dropped by the user on 2026-10-14 because it does not
  affect live trading (live sizes in Binance contract units; the gateway rounds to Kraken's).
- Runtime-owned accounting (ROADMAP 11): new `lib/src/account/position_ledger.*` (account-level
  positions flat to flat: average-cost basis, realized PnL, fees split on a flip, funding to
  the open position; the same rules as the dashboard's former Go rebuild). ExecutionState
  rebuilds it from `trading_fills` + `trading_funding` at every start, updates it per fill and
  writes `trading_open_positions`, `trading_closed_positions` and `trading_equity_daily`
  (`PostgresStateStore`; account marked at close(T) when T's market update arrives; missing
  days since the first fill are backfilled when replaying the fills from the initial cash ends
  at the live account). Failures are logged, never stop trading. Dashboard: Positions, the
  closed-positions CSV and the Overview equity curve read these tables (fallback to the old
  rebuild while an older ExecutionState runs); Live vs Expected keeps the PAPER comparison.
  Checks: `position_ledger_test.cpp` (the Go scenario, out-of-order rebuild, idempotent fills
  and funding; 14/14 component tests), Go tests incl. `TestRuntimeAccountingFeedsPositionsAndEquity`,
  and a run against a copy of the VPS PAPER data (21 fills): 5 open, 6 closed, realized
  -6,906.69 USD, fees 185.85 USD, equity 10-13 Oct backfilled (13 Oct cash 45,535.43 = the
  account), identical on a second start. Funding: user decision, none in the simulation;
  with Kraken, fees/funding updated while open and final values kept at close (ROADMAP 9).
  PostgreSQL "relation already exists" NOTICEs are silenced (`client_min_messages`).
  Full meson build, 14/14 component tests and step59 PASS with the unchanged fingerprint
  `94fdf8d8…` (RealTest 25/25).
- Chain LIVE mode (ROADMAP 6): `MODE=live` in chain.env makes `./algo` use profile `live`
  (`gateway-kraken`: venue mode, `/secrets/kraken.env`, symbol map, journal in
  `data/live/venue`, `LIVE_ORDER_SUBMISSION=disabled` by default) instead of `paper`
  (`gateway` + `sim-exchange`); all trading state moves to `data/live/` (ALGO_STATE) so
  PAPER is never mixed with real money; INITIAL_CASH comes from `LIVE_INITIAL_CASH`; the
  PAPER comparison is hidden. `algo` refuses MODE=live without the key file, namespace or
  cash, and refuses `up` while a container of the other mode runs. Dashboard API/watchdog
  accept the LIVE/KRAKEN/LIVE/venue boundary (Infrastructure shows Kraken via
  `gateway-kraken`; daily progress applies to it). Checks: rendered configs for both modes
  in a scratch installation; the PAPER render differs from HEAD only in data-init folders,
  `--periodic-reconciliation` and the profiles; Go test for the accepted/rejected boundaries.
  Not run against Kraken (no trading key).
- Periodic reconciliation (user request): ExecutionState compared with the exchange only at
  startup, so a lost fill or a manual venue change stayed unseen until a restart. With
  `--periodic-reconciliation` (added to `deploy/chain/compose.yml`) it also requests an exchange
  snapshot hourly 01:00-23:00 UTC and at 23:50 (wall clock; 00:00 skipped, the 23:50 result
  gates the daily cycle). A check is skipped while an execution cycle runs or any order is open.
  Mismatch: ALERT, trading paused as before, and the dashboard (which compares the latest
  retained snapshot) shows BLOCKED with its CRITICAL alert to Telegram. The next clean check
  resumes automatically (user accepted the recommendation); a clean check while already
  reconciled only logs "Reconciliation still clean". Without the option behaviour is unchanged.
  Dashboard `real_reconciliation.go` now uses the same size-relative quantity tolerance
  (`TestReconciliationQuantityToleranceScalesWithSize`). Side effect: the Reconciliation page
  becomes fresh every hour instead of PENDING between restarts. Checks: build, a scratch check of
  the schedule (24 checks a day, none at 00:00), all Go tests, step59 PASS with the unchanged
  fingerprint `94fdf8d8…`; first scheduled checks still to be seen on the VPS after deployment.
- Dashboard Execution shows why the venue skipped an order. The gateway rejects with
  `CODE: text`; the API (`classifyRejection` in `real_execution.go`) splits it: MARKET_UNAVAILABLE,
  MARKET_NOT_TRADING and BELOW_MINIMUM_QUANTITY become state SKIPPED (own "Skipped by venue"
  count, not in the reject rate, INFO alert `ORDER_SKIPPED`, neutral card with reason and what to
  do); NOTIONAL_MISMATCH and NO_REFERENCE_PRICE stay REJECTED (mismatch alert CRITICAL); unknown
  messages stay generic. Go test `TestVenueSkipsAreShownApartFromRejects`; all Go tests and the
  dashboard image build pass. Nothing produces these codes until the Kraken venue mode runs.
- Dashboard Infrastructure: the CPU/RAM/disk time-range picker moved to the page heading, as on
  Overview (same 1H/6H/24H/7D presets, From/To and drag-zoom); dashboard image build passes.

## VPS PAPER switched to volatility targeting (2026-10-13, 21:27-21:31 UTC)

User request: vol targeting instead of equal weight. Strategy unchanged
(`pure_rsi_paper_activity.json`, RSI 50/40); portfolio now `pure_rsi_vol_target.json`
(20% target volatility, 5-day covariance, 365 periods, threshold rebalance 2%, 1.5x limits).

- **Correction:** I had told the user the risk service would accept a new portfolio file.
  It does not: its PostgreSQL checkpoint is bound to the config text and a change stops it.
  New option `--adopt-portfolio-config yes|no` (compose: `RISK_ADOPT_PORTFOLIO_CONFIG`,
  default no) replaces the stored identity once, with an ALERT; earlier decisions keep their
  sizing. Runbook in deploy/chain/README.md "Changing the portfolio sizer".
- **Comparison:** `research/src/common/paper_baseline.cpp` now builds the sizer/rebalance
  exactly like the risk service (equal weight or volatility target, entry/exit-only or
  threshold), gives the volatility sizer the preceding closes (lookback + 1 + 5 days, as the
  risk service loads them) and can start from a seeded PAPER state (`start`: cash, positions,
  strategy signals/weights/virtual positions). `deploy/chain/comparison.py` reads
  `STRATEGY_FILE`/`PORTFOLIO_FILE`, archives the previous study on a configuration change and
  seeds the new one, keeping the earlier rows in comparison.json. It also had a latent bug:
  after any chain restart the snapshot lists `orders`/`processed_fill_ids` come back in another
  order, which it took as a changed account, so it had been stuck since today's restarts;
  lists are now compared sorted. `validation/paper_comparison_test.py`: 6 tests pass.
- Validation: build OK; 12 component tests; step59 PASS with the accepted fingerprint.
- **VPS:** release `/releases/20261013_212613-94df131-dirty` (comparison.py updated in place
  afterwards with the two fixes), backup `/backups/20261013_212743.tar.gz` and
  `/backups/chain.env.before-voltarget`; adopted with `RISK_ADOPT_PORTFOLIO_CONFIG=yes`
  (risk log: ALERT "Portfolio config changed on purpose", "config pure_rsi_vol_target.json,
  11 days of market history"), then back to `no` and risk restarted. Comparison archived the
  equal-weight study in `data/comparison/archive-27cf6e6266b2/`, new study seeded at decision
  2026-10-12 with 4 earlier rows; profile "PureRSI(7): entry > 50, exit < 40 · vol target
  20% · virtual USD". 13 containers up, no ERROR, account unchanged (45,535.43 USD, 5
  positions). Expected transient WARN until the 00:00 UTC decision: the dashboard Risk read
  model rejects the 2026-10-12 diagnostics because they carry the previous configuration
  identity. At the next close vol targeting resizes the open positions (threshold 2%).
- **Cleanup:** VPS keeps releases `193943` (rollback) and `212613`, backups (old stack
  archive, latest chain backup, chain.env before the switch), only the 6 chain images (390 MB
  dangling layers pruned), 47 GB free. Local: 70 obsolete images (old step/profile/paper/
  activity builds), all unused Docker volumes (named `algotrading-paper_*`/`dashboard_*`
  archived first in `storage/backups/local_docker_volumes_20261013.tar.gz`), unused networks,
  build cache older than 24 h and three older local releases removed.

## VPS: old stack removed, current work deployed (2026-10-13, 19:30-19:46 UTC)

User-authorized (roadmap step 8 plus deploying the new work).

- **Backup first:** `/backups/old_paper_stack_20261013.tar.gz` (186 MB, root-only, tar
  verified) holds every old Docker volume, `/opt/algotrading`, `/etc/algotrading`,
  `/var/log/algotrading` and the old `/home/ubuntu` scripts. The migration report
  (`/reports/migration-20261013_153532.json`) had already confirmed that everything the
  chain uses was copied with equal checksums.
- **Removed:** 63 stopped `algotrading-paper*` containers (4 projects), 30 named + 9
  orphan anonymous volumes, 11 old images (paper/activity/before-quote-volume, golang build
  images), `/opt/algotrading`, `/var/log/algotrading`, `/etc/algotrading`, old files in
  `/home/ubuntu`, the units `algotrading-paper`, `-paper-comparison`, `-paper-monitor` and the
  failed transient `algotrading-quote-clean/-final`, and release `20261013_153218`. Free
  disk 44 -> 47 GB. Kept: chain containers/volumes/images, apparmor `rsyslog.d/algotrading`
  (used by the chain receiver), releases `164959` (rollback) and `193943` (current).
- **Deployed:** release `/releases/20261013_193943-94df131-dirty`. `algo release` now packs
  the working tree for a dirty release (tracked + new non-ignored files; checked: no `.env`,
  no `storage/`), so the installed chain files match the images. `algo install`, all units
  copied, `algotrading-kraken` enabled (inactive: no key file), chain backup
  `/backups/20261013_194143.tar.gz`, `algo up`.
- **Checked after restart:** 13 containers up; no ERROR/ALERT line in any service; execution
  transition errors still 600 (all from the afternoon migration); sim-exchange snapshot
  unchanged (45,535.43 USD, 5 positions, 0 open orders); the 30 Hyperliquid registry alerts
  resolved by themselves.
- **Live finding fixed the same evening:** the first real symbol-map check raised a CRITICAL
  wrong-scale alert for ARCUSDT. ARC is the same token on both venues (Binance mark 0.0332,
  Kraken 0.0330) but fell ~45% today, and the dashboard compares yesterday's close with the
  current mark. The dashboard tolerance is now 5x (scale errors are >= 1000x); the gateway
  keeps 1.5x (prices minutes apart). New test with the ARC numbers. Only the dash-api image was
  rebuilt (`dash-api-symbolmap-tolerance.tar` in the release folder, checksum verified) and
  dash-api/watchdog/notifier recreated; the alert resolved. Active alerts now: 10 INFO (top-50
  assets Kraken does not list), no WARN/CRITICAL.
- Pending: first daily cycle at 00:00 UTC 2026-10-14 (now with vol targeting, see above); Kraken monitor waits for
  `/secrets/kraken_readonly.env`.

## Kraken account monitor and Telegram in the chain (2026-10-13, local)

The warning-only Kraken account monitor (`live_trading/kraken_account_monitor.py`, read-only
key, 30 s polls, warnings + 09:00 UTC daily portfolio) ran only on the PC with a
self-started notifier container. It is now part of the chain:

- `deploy/chain/systemd/algotrading-kraken.service`: host python3 runs the monitor from
  `/chain/tools` with `--credentials /secrets/kraken_readonly.env`, policy
  `/config/kraken_account_policy.json` (installed once from
  `config/venues/kraken_futures/account_policy.json`: USD 50 / 10% / 0), state
  `/data/kraken_account`, `--retention-days 3`. `ConditionPathExists` keeps it off until
  the key file exists.
- `compose.yml` service `kraken-notifier` (profile `kraken`, chain dash-api image,
  `/dashboard-alert-notifier`): reads `/data/kraken_account`, sends INFO and above to
  Telegram (`KRAKEN_NOTIFIER_SINK`, default TELEGRAM), own receipt store
  `data/dashboard/kraken-notifier`, bridge network only. Enabled with
  `COMPOSE_PROFILES=kraken` in chain.env (checked: without it the service is absent).
- Monitor: new `--credentials`, `--state-root`, `--retention-days` (0 = keep all, the local
  default, so local history is untouched). Pruning deletes only raw samples, hourly; alert
  state/events and per-day portfolio snapshots stay.
- `algo install` copies the three monitor scripts; `algo ps` reports the monitor state.
- Tests: `kraken_account_monitor_test.py` 14 pass, including a new retention test and the
  Docker notifier contract now run against `algotrading-dash-api:chain`
  (`KRAKEN_MONITOR_DOCKER_TEST=1`); account, portfolio and shadow tests pass.

Not on the VPS: it needs the read-only key file and Telegram settings there and the
user's authorization (runbook in deploy/chain/README.md).

## Pipeline cycle check redone; RAM and disk in percent (2026-10-13)

- The Pipeline "Cycle Proof" panel only appears with real data (PAPER on the VPS), so the
  earlier redesign never saw it. Its styles were in `src/styles.css`, which nothing
  imported, so values ran together ("5/50 cancel request(s)" = "5/5" + "0 cancel
  request(s)") next to internal table names. It is now "Last Cycle Check · <day>": one
  line of counts ("5 order(s) planned · 5 at the exchange · 5 filled") and one chip per
  stage (Market Data > Strategy > Risk > Planning > Orders > Fills > Reconciliation) with
  the evidence in the tooltip and listed only for stages that are not aligned. The
  dashboard's own "Dashboard Projection" stage is left out. `src/styles.css` (dead) was
  deleted. Checked with a mock-mode screenshot.
- Infrastructure RAM and disk charts show percent of the host total (the total stays in
  the panel title); RAM "By container" is each container's share of host RAM. Compiles;
  not yet seen with real host history.

Not deployed to the VPS.

## Binance -> Kraken symbol map with price checks (2026-10-13)

The user asked for an alert whenever the program cannot match a strategy symbol to the
exchange, including scaled contracts (Binance 1000PEPEUSDT = 1000 PEPE, Kraken PF_PEPEUSD
= 1 PEPE). The old Hyperliquid TESTNET symbol registry (dashboard only, unused by the
gateway, broken on the VPS because no Hyperliquid URL is configured) produced 30 alerts
that meant nothing for PAPER or Kraken; it has been removed.

- **One table:** `config/venues/kraken_futures/symbol_map.json`, 161 of 523 Binance
  perpetuals, all 20 current universe assets, five scaled (PEPE, SHIB, BONK, FLOKI x1000,
  MOG x1,000,000). Built by `tools/kraken_symbol_map.py` from public instruments plus a
  same-moment mark-price ratio check (5%); no rejections on 2026-10-13.
- **Gateway:** `venue/symbol_map.*` replaces the hard-coded `SCALED_ASSETS` list
  (`--symbol-map` is required in venue mode). New order-time guard: venue USD value
  (venue units x Kraken mark from public tickers, cached 60 s) must be within 1.5x of
  source quantity x `ExecutionOrder.reference_price`, which the planner now sets from
  its sizing price (optional in the order JSON). Otherwise `NOTIONAL_MISMATCH` + ALERT;
  no price, `NO_REFERENCE_PRICE`. `algotrading_venue_adapter_test`: 85 checks pass,
  including a wrong-scale table and file validation.
- **kraken_shadow.py** reads the same table (`NOT_IN_SYMBOL_MAP`);
  `validation/kraken_shadow_test.py` 14 tests pass.
- **Dashboard:** `/api/symbol-map` and a new Symbol Mapping page. Every minute it compares
  the latest Binance close with the Kraken mark for the universe and the top-50 ranking.
  An unmapped symbol is `MISSING_FROM_TABLE` when Kraken lists a same-named contract (or
  without its 1000/1000000/1M prefix) whose price confirms the scale: the table is out of
  date and must be rebuilt (a hint only; the gateway trades only the reviewed table).
  Alerts: `SYMBOL_MAP_SCALE_MISMATCH` (CRITICAL), `SYMBOL_MAP_SIGNAL_NOT_TRADEABLE`
  (CRITICAL), `SYMBOL_MAP_STRATEGY_NOT_TRADEABLE` (WARN), `SYMBOL_MAP_TABLE_OUT_OF_DATE`
  (WARN, top-50 asset Kraken lists but the table lacks), `SYMBOL_MAP_RANKING_NOT_TRADEABLE`
  (INFO, top-50 asset Kraken does not list; about a dozen permanently, so not a warning),
  `SYMBOL_MAP_UNAVAILABLE` (WARN).
  Global readiness has a non-blocking `symbol-map` requirement; Manual Control (real
  provider) uses the map instead of the registry. Go tests pass; the page was checked with
  a mock-mode screenshot.
- **Chain:** `algo install` copies the table to `<root>/chain/config/venues/kraken_futures/`;
  dash-api and watchdog mount it as `/config/symbol_map.json`.
- step59 PASS with the accepted fingerprint; 12 component tests pass.

Not deployed to the VPS. Remaining 7b items are in docs/ROADMAP.md.

## Overview shows several alerts (2026-10-13)

The Overview Alerts panel is now a table (Severity, What happened, Area) with the most
severe and then most recent active alerts. It shows as many rows as the Active Positions
table, at least 3, with the same 40px row height, so both panels are about the same size.
The rest are behind "+N more →" and the full detail is in the row tooltip. Checked with a
Playwright screenshot against the mock API. Not yet deployed to the VPS.

## VPS migrated from the old PAPER stack to the chain layout (2026-10-13)

Authorized by the user after committing (94df131). Release
`/releases/20261013_153218-94df131-dirty` (images.tar 240 MB, source.tar.gz from
`git archive HEAD`, SHA256SUMS verified on the VPS; "dirty" = `algo release` itself now
packs `git archive HEAD` instead of a working-tree tar that would carry ~1.8 GB of ignored
data). At 15:34 UTC: images loaded, `algo install /`, old units `algotrading-paper`,
`-monitor`, `-comparison` stopped and disabled (containers and volumes kept as fallback),
`migrate_paper.py --apply`: every volume copied with equal checksums (PostgreSQL 63 MB,
NATS, market SQLite, watchdog/audit/alerts/notifier), comparison (22 files) and Kraken
public study (27 files) copied, 50/40 settings in chain.env; PostgreSQL 15 fills / 5
positions. Log receiver/cleanup and chain/monitor/comparison units installed and enabled.

Verified after `algo up`: 13 containers up (dash-api/web/notifier/watchdog/postgres
healthy), `/logs/<service>/20261013.log` filled by the receiver, strategy and risk restored
their 2026-10-12 checkpoints, sim-exchange restored cash 45,535.43 USD with 5 positions and
0 open orders, dashboard on 127.0.0.1:8092 (same SSH tunnel), comparison history (4 days)
kept, `/reports/closed_positions*.csv` written from the migrated fills.
Startup note: the chain creates its JetStream consumers anew, so execution-state re-read 30
old order updates (e.g. ACCEPTED for FILLED orders); each was refused 20 times (max_deliver)
= 600 "Invalid order status transition" errors in 2 s, then dropped; durable state comes
from PostgreSQL and duplicate fills are deduplicated. Strategy/risk/planner keep their
first stale messages pending until the next UTC day.
Root cause and fix (local, not yet deployed): the runtime image ships nats.c 3.2, where a
durable consumer created implicitly by `js_PullSubscribe` is deleted when the subscription
is destroyed at shutdown (the code assumed >= 3.4). Every clean service stop deleted its
consumers, so every restart recreated them and re-delivered the retained stream (the old
VPS NATS volume had empty `obs/` folders after its stop). `JetStreamBus::subscribe` now
creates missing durables with `js_AddConsumer` and always binds explicitly, so nats.c
never owns or deletes them; checked against a real NATS 2.10 (old: consumer gone after
destroy; new: kept and re-bound). Defence in depth: `OrderManager::onOrderUpdate` returns
false for a stale echo (a non-terminal status the order has passed, e.g. ACCEPTED after
FILLED), `ExecutionEngine::processExchangeEvent` passes it up and execution-state acks it
with a DEBUG line; other invalid transitions (FILLED then REJECTED) still throw. New
`validation/order_manager_stale_update_test.cpp` (in mock_component_tests.sh). Results:
12/12 component tests, step59 PASS `94fdf8d8…`, transport_persistence_integration_test PASS,
all chain services build.
Deployed on the VPS the same day at the user's request (release
`/releases/20261013_164959-94df131-dirty`, images only, 16:51 UTC). First restart: the old
code deleted the consumers one last time; the replay was silent (0 new transition errors,
duplicate submits ignored by sim-exchange, cash 45,535.43 USD / 5 positions unchanged).
Second restart: 18 consumers before and after, none recreated, 0 duplicate submits,
0 new errors, 13 containers up.
Pending: check the first chain daily cycle after 00:00 UTC 2026-10-14; after some accepted
days delete the old stack leftovers (README "After some accepted days"); move the Kraken
account monitor/Telegram into the chain.

## PureRSI 80/70 with 20% volatility target on Binance perpetuals, 2025 to date (2026-10-12)

Research study at the user's request (fast research Backtester via
`algotrading_research_html`, which gained `--sizer volatility-target`; defaults unchanged).
Data: new `tools/binance_perp_daily.py`, 655 USDT perpetuals (523 TRADING, 132 SETTLING,
GAIB without candles) 2024-12-07..2026-10-11, volume = USDT quote volume, in
`storage/databases/1d_binance_perp_quote_20241207.csv`. Rules: RSI(7) entry > 80 / exit < 70,
top 20 by SMA(quote volume, 25), max 10 signals; volatility target 20%, covariance of 5
returns, 2% rebalance threshold, 1.5x gross. Close(T) decisions, next-open fills.
Results (`storage/backtests/sensitivity_results/pure_rsi_voltarget20_binance_perps_2025_fee*/reports/PureRSI.html`):
zero fees +33.6% (17.0% annualized), max DD 14.7%, Sharpe 0.85, 160 trades, 35% win
rate, profit factor 1.47; with 0.05% taker fee +30.6%, max DD 15.0%. BTC over the same
period -10.5% (max DD -53%). Not modelled: funding, slippage, delisted contracts already
removed from exchangeInfo (survivorship), Kraken availability.
`validation/research_html_report_gate.py` PASS (default equal-weight behaviour unchanged);
its `algotrading_research_stats` summary regex was updated to the readable-log format
introduced on 2026-10-11 ("return 12.34%, ..., 56 trades"), which it no longer matched.

## Manual rebalance executes immediately; closed positions, fees/funding and tax CSV (2026-10-12)

Manual Control semantics changed (supersedes the "held until released" model below):
a confirmed CSV is executed by the replay at the next price it has — the open for
requests that arrived overnight (it replaces the pending strategy plan), a point just
before the close (`YYYYMMDD5`, close prices, synthetic flat bars) for daytime requests —
through `ReplayRuntime::rebalanceToManualTarget` (same planner/venue path; evidence
counters untouched). The next close re-syncs every strategy to its risk-approved weights
(`resyncToApprovedTargets`: HOLD positions are resized, not only entries/exits), so the
manual positions last until the end-of-day decision. Only the newest queued request runs;
older ones are recorded as SUPERSEDED. Release endpoint/button removed.
Limits now come from the portfolio-risk config (`ReplayRuntime::manualLimits()`: minimum
over strategies of `RiskConstraints` max gross / max asset; Pure_RSI 1.5x / 1.5), published
in state.json and enforced by the API and the runtime; above them the request is
rejected, never clipped. Previously a hard-coded 100%.
Manual Control page: only upload + format rules, preview, and a progress bar per request
(Sent -> Picked up by replay -> Orders at exchange (n/m filled) -> Filled -> Back to
strategy targets), fed by `requests[]` in `/api/manual-control` (order ids recorded by the
replay in the processed request).
Verified: short repro (overnight request executed at the open replacing the plan, closed at
the next open by the re-sync) and browser run on the full replay (daytime request: 3 orders
filled at the close, RESYNCED). step59 gate PASS (`94fdf8d8…`, RealTest 25/25).

Positions: open positions gain Opened, Fees, Funding (only when the source records it)
and Net PnL (unrealized + realized so far - fees + funding); new Closed Positions table
(flat to flat: entry/exit, gross PnL, fees, funding, net PnL, net %) with a CSV download;
cards Realized PnL (net) and Total Fees. Shared `position_history.go` rebuilds positions
from fills (average cost, partial closes, zero crossings split the fee), fees
(TRADING_FEE/REBATE by fill) and funding (by asset, to the open position). Simulation:
whole MOCK stream; PAPER/LIVE: all `trading_fills` (`TradeHistoryFills`, up to 200,000;
funding not persisted there, shown as unavailable).
Cross-check on one replay snapshot (2021-03-01, 327 fills): 100k + realized - fees + funding
= MOCK cashTotal exactly (181255.4164).

Reports: `GET /api/reports/closed-positions.csv[?year=YYYY]` and, with
`DASHBOARD_REPORTS_DIR`, `closed_positions.csv` + `closed_positions_<YEAR>.csv` rewritten
every 10 minutes (atomic; not written if the history is incomplete). Chain: `dash-api`
mounts `<root>/reports` (`ALGO_REPORTS`, owned by uid 70 via data-init). Local simulation:
`deploy/historical_replay/run/reports/`.

Full history with the new binary (2026-10-12): control `replay.py system` without manual
requests (`system_control_20261012`): 628 fully matched, the same four known differences
(BNB exit date, FET missing + extra, final ZEC), 1261 fills, i.e. identical to the earlier
baseline. The dashboard run with one manual test request on 2020-01-29
(`dashboard_manual2_20261012`): 625 matched / 11 differences = those four + four from the
request itself (ETC sold at the close and re-bought by the re-sync, BTC/ETH bought and
closed by the re-sync) + EOS 2021-08-08 missing and LUNC entering 2021-12-23 instead of
2021-12-22.

Root cause of EOS/LUNC (a real bug, fixed): signals and decisions were identical with and
without the request (pipeline compared on 2021-08-07); only equity differed by 0.06%, so
order quantities changed slightly. The EOS order (451) and the LUNC order (554) were
created but MOCK rejected them at admission: grid units were converted to double by a
long-double division followed by a cast to double (two roundings), which leaves about
one 8-decimal quantity in 4,000 one ULP off the grid, so `divisibleByIncrement` failed
(SIZE_INCREMENT). The strategy kept its signal with no position and retried the next day
(LUNC) or exited first (EOS). Fix: `MockVenue::unsignedScaledToDouble` / 
`signedScaledToDouble` in `lib/src/exchange/mock/decimal.h` divide two exact doubles
(single IEEE rounding) up to 2^53 units, used by `ReplayRuntime::unitsToDouble`,
`ManualControl` order building, MOCK matching/account and the canonical replay.
Check: 6 million random grid values up to 10 million units round-trip exactly (the old
path failed 0.02-0.025%). Above ~10 million units (16 significant digits) a double cannot
hold 8 decimals at all; relevant for very cheap coins (see ROADMAP 7b).
Evidence: short dashboard replays with the same manual request reproduce the missing
EOS order before the fix (order id gap 450 -> 452) and, after it, EOS fills on
2021-08-08 and LUNC on 2021-12-22 (274 matched, only the 4 request trades differ).
step59 gate PASS after the fix with the unchanged fingerprint `94fdf8d8…` (RealTest 25/25).

Reconciliation page (2026-10-12): cards Status (with a plain explanation), Last Checked,
Assets Compared (how many differ) and Open Orders; table "Positions: Ours vs Exchange"
(Ours, Exchange, Difference, Open Orders, Status); "Problems Found" only when the
reconciler reports issues (kinds in plain words); Open Orders. The fixed-text panels and
the Target/Effective columns were removed. The simulation used to copy the exchange
quantity into both columns; it now compares the reconciler's local ledger with the MOCK
account (Go test added). Go tests/vet and the web build pass.

Historical step gates (2026-10-12): every numbered gate from step47a to step58 except
47b failed at its frozen-checksum stage (files moved/changed by the restructure), not on
behaviour. New `validation/mock_component_tests.sh` builds the current lib sources once
and runs the 11 component tests (step47a-step57 `*_test.cpp`, -Werror): all PASS.
`step58a_canonical_replay_suite_gate.sh` lost only its checksum stage and its mode list
was updated (`{resources,fast,system,dashboard}`); it passes, including the frozen Step56A
mock-default 107-day economics. `step47c` now depends on 47b only. The retired gate
scripts (47a, 48-57, 56a, 58) were deleted with the user's authorization and the `.ai/`
index was regenerated (`generate_ai_index.py --check` PASS).

The old local `deploy/paper_trading/monitor.py` (project algotrading-paper, writing
storage/paper_trading/telemetry) was stopped in WSL on 2026-10-12; the Kraken account
monitor and the chain `deploy/chain/monitor.py` keep running.

Pending after this session (details in docs/ROADMAP.md): user review and commit. New recorded requirement (ROADMAP 7b): source/venue contract
units, e.g. Binance `1000PEPEUSDT` (per 1000 PEPE) versus Kraken `PF_PEPEUSD` (per 1
PEPE); today a hard-coded `SCALED_ASSETS` list in `venue_protocol.cpp` (copied in
`kraken_shadow.py`) handles it. Not implemented yet.

## Manual Control works in the replay; Alerts & Audit and Live vs Expected rebuilt (2026-10-12)

Superseded for Manual Control semantics by the entry above (no persistent manual target,
no RELEASE); the fixes and the Alerts/Live vs Expected/Infrastructure notes still apply.

Manual Control (replay simulation): the operator uploads `asset,weight_pct` (Excel `;` /
decimal comma accepted, CASH optional, total <= 100%, long only), previews it against the
current account (BUY/SELL/CLOSE per asset; held assets missing from the file are closed)
and sends it. dashboard-api queues `REPLAY_MANUAL_V1` in `<state dir>/requests`; the
canonical replay applies it before the next close (`applyManualRequests`), and
`ReplayRuntime::setManualTargetWeights` then replaces the approved strategy decisions at
every close (signals/risk still run; same planner -> MOCK path; per-strategy reference
capital kept) until "Return control to strategies" queues `action=RELEASE`. Requests are
checked again in C++ (explicit MOCK route, close available, weights). Processed requests
get `result/detail/replay_date` appended and form the Alerts & Audit log. Requests are
cleared at replay start; a resume checkpoint refuses to snapshot an active manual target.
A manual target rebalances to its exact weights at every close.
Verified end to end in the browser on the full-history dashboard replay (SET applied at the
next close, positions 30%/20%, RELEASE applied, audit rows present). Not for PAPER/LIVE:
the real provider still fails closed.

Fixes found on the way:
- `MockVenue::mulDivRoundNearestU64` now uses an exact 128-bit product. The old
  decomposition overflowed when the denominator was large (weighted average entry price
  divides by the position quantity), so increasing an existing position above ~43 coins
  at 8 decimals rejected the fill ("matcher emitted fill rejected by Step52 account").
  Strategy trades (enter/exit whole) never increase a position, so the canonical replay
  was unaffected; results are identical wherever the old code succeeded. That error now
  names the asset, order, quantity and price.
- state.json `routeSafe` now reports `ReplayRuntime::routeSafe()`. It used
  `canRouteNewSubmit()`, which is false whenever events arrived since the last
  reconciliation (almost always), although the runtime reconciles before each submit.
- (Resolved later the same day: see "Historical step gates" above.)
  `validation/step52_mock_account_margin_positions_accounting_gate.sh` is stale: it hashes
  files from the old layout (`lib/src/exchange/mock_decimal_grid_v1.h`) and fails before
  testing anything.

Alerts & Audit: cards Critical / Warnings / Info (and "Seen by Operator" when ACK
exists); one "Open Alerts" table (Severity, What happened + detail, Area, Since, Status,
"Mark as seen"); Alert History only when lifecycle events exist; "Audit Log" (When, Who,
Action, Result, Detail). Watchdog/step panels and banners removed. The simulation
derives notices for route safety, reconciliation, leverage and drawdown (same scales as
Overview/Risk, with the date the condition started), rejected orders and manual-target
state. Alerts remain notices only.

Live vs Expected: cards Equity, Expected, Difference ($, %), Largest Gap, Days Matching
(within ±0.5%); "Equity vs Expected" and "Difference (%)" charts with one shared range;
Positions vs Expected (PAPER) and a Daily Comparison table (signal mismatches column only
when non-zero). The Step 45 behaviour-baseline cards, contract and anomaly panels were
removed from the page.

Infrastructure tiles show only name and state; CPU, RAM and Disk are separate panels.

Validation: step59 gate PASS with fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2` (RealTest 25/25) after
the runtime, replay and decimal changes; Go tests and web build pass.

## Market Data and Infrastructure simplified; local host telemetry (2026-10-12)

Market Data: cards Latest Candle, Healthy Assets, Missing Candles, Duplicate Timestamps,
Gaps, Invalid Rows; one "Freshness" table (Asset, Last Candle, Age, Status; unhealthy
first, filter, 20 rows + "Show more"). Source/active-signal/universe cards, the integrity
panel, universe & candidate diagnostics and the strategy snapshot were removed (strategy
detail lives in Pipeline). Replay age shows `0d`.
Infrastructure: one tile per component (containers, trading processes, and PostgreSQL /
NATS / exchange when the provider uses them) showing only the name and Up / Degraded /
Down / Unknown (green, orange, red, grey); then one shared time-range picker and CPU, RAM
and Disk each in its own panel (default "Last 1 hour", captions removed).
All Step 32-37 venue/readiness panels, API diagnostics, readiness, outbox and tables were
removed from the page (endpoints unchanged).
The local replay dashboard now has host telemetry: `research/replay.py dashboard` starts
`deploy/chain/monitor.py --project dashboard --services dashboard-api,dashboard-web`
detached (pid in `deploy/historical_replay/run/telemetry/monitor.pid`, replaced on the next
launch); `docker-compose.simulation.yml` mounts that folder and sets
`DASHBOARD_HOST_METRICS_FILE/PROJECT` and `DASHBOARD_HOST_HISTORY_FILE`; the simulation
provider reads the snapshot for the VPS and container rows. `monitor.py` accepts the
project name `dashboard`.
Page subtitles (the explanatory line under each title) were removed from every page.

## Risk page redesigned; dashboard outage hardening (2026-10-12)

Risk (`risk_view.go`, shared by replay and PAPER): cards Leverage (green <= 1.5x, orange
<= 2.5x, red above), Gross/Net Exposure % of equity, Largest Position, Drawdown and Max
Drawdown (% below the running equity peak); Leverage and Drawdown daily charts with one
page time range; "Strategy Limits" per strategy (capital, sizer, max asset, max gross,
gross after asset caps, gross scale, approved gross, binding limit: None / Asset cap /
Gross cap); "Exposure by Asset" (weight, approved weight, account-level asset cap =
max(strategy cap x capital share), cap usage bar). Display only, no new limits. Replay
equity points carry `gross` (display only) for the leverage history; PAPER has no daily
gross, so only the drawdown chart (from the comparison equity) shows. Old evidence
panels/message IDs remain only as a fallback when no view data exists.
Outage hardening: the API returned sporadic 503s because state.json can be read half
written (tmp+rename is not atomic on the Docker Desktop bind mount); the simulation
provider now retries 3x and serves the last good snapshot when it is < 10 s old. The web
`useDashboardResource` keeps the last good data on a failed refresh and retries every 2 s;
the error page only shows when nothing loaded. Stress test: 1000/1000 OK (was 6/600 503).

## Execution page simplified (2026-10-12)

Cards: Open Orders, Filled Orders, Rejected, Total Fees (latency/slippage cards and the
"Step 42 ledger foundation" metrics removed). "Average Slippage" chart: daily
notional-weighted slippage in bps, fill price against the order's expected price (open of
the execution day, the order limit price); positive = adverse. Replay computes it in Go
(`simulationSlippage`, `slippageHistory` in `/api/execution`); realtest-parity fills at the
expected open, so it is 0 bps there. PAPER has no expected price persisted: "No slippage
data". Orders table: Order ID, Asset, Side, Qty, Filled, State, Expected, Avg Fill,
Slippage, Fees, Submitted (newest first, 20 rows + "Show more"). The ledger is now
"Fills": Time, Order ID, Asset, Side, Qty, Price, Notional, Fee (entry id, fill id, hash
and cash delta removed). Replay times show as `2021-05-20 open`. Rejects and
cancels/replacements panels appear only when non-empty; order rows are not clickable (the
order inspector was removed at the user's request). The simulation execution read model no longer scans every fee x
fill per order (was cubic over the full history); per-order fees/fills use maps.
Caddy fix (both Caddyfiles): `header` runs before `try_files`, so the old
`header /index.html no-cache` never matched "/" or SPA routes and browsers kept a stale
index.html pointing at an old bundle after redeploys. Now everything outside `/assets/*`
is served `Cache-Control: no-cache` (verified on `/` and `/execution`).

## Pipeline page finished; Overview PnL/Leverage colours (2026-10-11)

Overview cards: PnL green when >= 0, red when negative; Leverage green up to 1.5x,
orange up to 2.5x, red above (display only; no limits). Positions' Unrealized PnL card
uses the same colours.
Pipeline works locally with the replay (no live/PAPER services needed). The replay
dashboard mode turns on `ReplayRuntime::enableDecisionDiagnostics()` (keeps the last
signal batch, DecisionBatch and `PortfolioRiskEvaluation`; off by default, collection only)
and `StrategySignalEngine::indicators()` (read-only), and writes a `pipeline` block per
close: liquidity rank and top-20 membership and RSI rank (same `TopNLiquidityUniverse`/
`IndicatorRanker` classes), RSI(7), signal, sized/capped/approved weights, rebalance action,
target USD, position at decision time, planned orders. Go maps it to the decision matrix
(Asset, Liq. Rank, RSI(7), Signal, Sized W., Approved W., Decision, Target, Current, Delta,
Order, Execution), a 12-stage rail and the Why? trace; execution state comes from the
routed venue order with the same id. PAPER pipeline now fills sized/approved weights and
the decision from the persisted risk evaluation; RSI/rank stay "—" (not persisted by the
strategy service). Page restyled like Overview: "Pipeline" heading, summary cards, no
source banners; the proof panel ("Cycle Proof") appears only for service cycles. Go
tests added (replay lineage, dollar format, PAPER risk evaluation).
Multi-strategy breakdown (user request): the matrix has no RSI column and shows the
aggregated signal in [-1, +1] (0 = flat) = sum of strategy signals weighted by capital
share (strategy reference capital / total). Expanding an asset shows one row per strategy
(capital, weight in signal, signal, its own indicator, sized/approved weight of its capital,
decision) plus the aggregated row, then the stage rail. Matrix Sized/Approved W. combine
the strategies by the same share (fraction of total capital). The replay `pipeline` block
carries `strategies` per asset; PAPER builds the same breakdown from the persisted signal
batch and risk evaluation (indicator "not persisted").

## Positions page finished (2026-10-11)

Same treatment as the Overview: no captions or source banners, amounts like `30,771$`.
Table: Asset, Side, Qty, Notional, Entry, Mark, Current W., Target W., Δ Weight, PnL,
PnL %, Status (last). Current W. = position value / equity; Target W. = latest approved
risk target / equity; Δ Weight = target − current (what the next rebalance still changes).
Replay: state.json carries `targets` (latest decision's target USD per asset, display only).
PAPER: positions are now valued (`real_position_valuation.go`): marks = latest completed
close in the market SQLite (`sqlite.Reader.LatestCloses`), equity = cash + marked
positions, targets from the latest PortfolioRisk decision, entry/PnL = average cost of
persisted fills when the whole fill history fits the 1000-row read, per-position status
from reconciliation; anything missing stays "—". The PAPER Overview uses this valuation
for Total Equity and Leverage. Go tests added (average cost, weights/PnL, no marks).

## Overview redesign requested by the user (2026-10-11)

Top bar keeps only the SSE state and the alerts bell (Mode/Exchange/Data/Trading/
Reconciliation/Readiness and the UTC/business label removed). Overview: no captions;
figure cards Total Equity, Cash, Unrealized PnL, Leverage (gross position value / equity,
replaces Margin Used; PAPER uses the comparison positions or the approved gross target);
System Readiness, Reconciliation panel and Recent Events removed. Equity Curve: Balance
(blue, equity minus open-position PnL) and Equity (orange), no reference line, axis values
like `300,000$`, plus a Grafana-style "Close loop" state timeline (green CLEAN / red
otherwise). Live vs Expected (Overview panel
and page): equity against the fast backtest; the replay dashboard now runs the fast
backtest of the same window first (`expected_equity.csv` via `ALGOTRADING_REPLAY_EQUITY_CSV`),
PAPER keeps the daily comparison. Active Positions: Asset, Side, Notional, Status, PnL.
Alerts panel (replaces Recent Events): most severe then most recent active alert, "+N more", "No active alerts" when none. Close loop is its own full-width state-timeline panel; one page-level time range (Grafana-style dropdown button, e.g. "Last 7 days", with presets and From/To; used by every time picker) drives every Overview chart and a drag-zoom on any chart zooms all of them; Positions column order Asset, Side, Notional, PnL, Status; top bar band removed (SSE state and bell float top-right).
Replay state equity points carry `balance` and `closeLoop` (display only).
Real PAPER Overview still lacks per-position marks/PnL and balance history (roadmap 11).

## VPS PAPER -> chain migration prepared and rehearsed locally (2026-10-11)

Read-only VPS inventory (no changes): the 50/40 study runs as `algotrading-paper` with the
activity overlay (PostgreSQL/NATS in `algotrading-paper-activity-*` volumes, study since
2026-10-10 15:37 UTC), comparison collector and monitor as host units, logs under
`/var/log/algotrading`. Leftovers: three stopped hashed projects with their volumes, old
archives in `/home/ubuntu`, two failed `algotrading-quote-*` units. Notifier test sink and
watchdog events grow ~5 MB/day each.

Prepared: `deploy/chain/migrate_paper.py` (copy-only, discovers sources from the old
containers, SHA-256 per file, copies the dashboard password without printing it, sets the
chain PostgreSQL password, rewrites comparison paths), `algo release` (images + source +
SHA256SUMS), `chain/systemd/` units (chain, monitor, comparison), comparison collector moved
to `deploy/chain/comparison.py` (container names/paths configurable; `kraken_shadow.py
--study-root`), dashboard comparison panel in chain via `PAPER_COMPARISON_FILE`.

Rehearsal: consistent snapshot streamed from the VPS (pg_basebackup, SQLite online backup,
volume tars) into a stopped local replica, migrated into a fresh installation, `algo up`:
strategy/risk restored the 2026-10-10 checkpoint, simulated exchange restored cash
59,864.40 USD, 4 positions, 0 active orders; the dashboard shows the same fills, positions
and comparison; the fast baseline recomputed from the new paths with identical rows.
Strategy and portfolio files are byte-identical to the VPS. paper_comparison_test (2) and
kraken_shadow_test (13) pass. VPS execution pending authorization (runbook:
deploy/chain/README.md).

## Dashboard charts: Grafana-style host resources and equity (2026-10-11)

User requests: Infrastructure CPU/RAM/disk as Grafana-style time charts with an overall line and a
per-container breakdown in the same chart; the Overview equity (and Live vs Expected) charts in the
same style. Final design after the user's review: plain lines without area fill; "By container" shows
one line per container (user decision: no grouping unless more than 30 containers; identity = 7 validated hues x 5 dash patterns; clicking a legend entry isolates that line, clicking again shows all); the By-container tooltip lists containers from highest to lowest value; zooming keeps the chosen view (a stale hover index after a zoom re-fetch used to crash the charts back to Overall); time
range chosen Grafana style: presets (infra 1H/6H/24H/7D ending now and refreshed every 30 s; equity
7D/1M/6M/1Y/ALL) or an explicit From/To in UTC, and dragging across any chart selects a span (zoom
in, also written into From/To); double-click returns to the default range. Value axis on the right
fits the visible data; crosshair tooltip lists every series.
Source: `dashboard/src/components/TimeSeriesChart.tsx`, `TimeRangePicker.tsx`, `HostResourceCharts.tsx`,
`LineChart.tsx`; colors from the validated categorical palette for the dark panel surface (#0d1826).
Data: `deploy/chain/monitor.py` appends `history.jsonl` (30 s rows, 7 days; container CPU divided by
host cores); `GET /api/infrastructure/history?range=…` or `?from=…&to=…` (RFC 3339, within the
retained week; at most 720 averaged buckets; absent values stay absent; stale flag), served by
dashboard-api `internal/server/host_history.go`; chain compose sets `DASHBOARD_HOST_HISTORY_FILE`.
`algo` starts the monitor with `setsid` and waits for its first sample (a background monitor started
from a short WSL command used to die).

Evidence (local chain): Go tests all PASS (history test covers bucket means, absent values, stale
flag and explicit windows); web/API images build (TypeScript check); host_monitor_test OK (1 skip);
Playwright screenshots of the real Infrastructure page (overall, by-container lines, drag-select
re-fetching 10:27–10:39 at 30 s resolution) and of the Overview equity chart with mock demo data
(hover, 7D, drag-select), no page errors. On WSL, Docker Desktop containers run in a separate VM from
the measured host. Not deployed to the VPS.

## Real-venue adapter and Kraken Futures protocol — offline and read-only (2026-10-11)

User request: one exchange class whose venue-specific parts are replaced per exchange;
Kraken only for now; no orders (the current key is read-only). Implemented in
`live_trading/exchange_gateway/src/venue/` (guide: live_trading/exchange_gateway/README.md):
common `LiveVenueAdapter` (implements ExchangeGatewayAdapter), `VenueProtocol` interface,
`KrakenFuturesProtocol`, SQLite `VenueJournal`, libcurl transport with `GetOnlyTransport`
guard. Gateway mode `--mode venue` (order submission disabled by default: local rejection
and GET-only transport). Rules: journal before ACK, SENDING committed before the request,
ambiguous submit -> UNKNOWN resolved only by open orders/fills (never resent), not-processed
errors resent, fill de-duplication with stable local FillIDs, unavailable/expired markets and
sub-step quantities rejected without substitution, quantity rounded down, scaled Binance
coins converted (x1000/x1e6) and back, keys with transfer/withdraw refused. OpenSSL is a
new dependency of the gateway only; libalgolib unchanged. Sources: docs.kraken.com Futures
REST reference (sendorder, cancelorder, openorders, fills, orders/status, batchorder,
instruments), authentication and rate-limit guides; unconfirmed points are listed in the
guide (trading-key permission value, mkt response details, clientOrderIdAlreadyExist scope,
cancelallordersafter, fees absent from fills).

Evidence (WSL): whole-project build PASS; offline `algotrading_venue_adapter_test` PASS
(16 groups, 72 checks: signing equal to the Python reference, credentials, catalogue,
disabled submission never posts, accept/fill/restart without duplicates, scaled coin,
local rejections, ambiguous and crash-while-sending recovery, not-processed resend,
held/cancel, partial fill, unattributed/reused-id fills, snapshot, permissions); log
storage/kraken_shadow/venue_adapter_test.log. Real read-only probe (`--live-read-only`,
GET only): key read yes/trade no/withdraw no, 201 PF markets, accounts/positions/open
orders/fills parsed (Kraken accepted the C++ signature). Step59 PASS, fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2 (storage/kraken_shadow/venue_step59.log).
Audits PASS: exchange_gateway_time_handler, live_deploy_topology, live_contract_identity,
no_legacy_clock. No order, cancel or other POST was sent; VPS unchanged.
Known gaps: fills publish commission 0 (Kraken fills have no fee; roadmap 9); snapshot cash is
the USD wallet balance (collateral/margin: roadmap 8); venue mode is not yet in chain/compose.yml.

## Runtime root and chain launcher — implemented and validated locally (2026-10-11)

User decisions: one installation per machine; on the VPS the root is `/` itself with
/chain (compose + launcher), /config, /secrets, /data, /logs, /reports, /backups,
/releases; short names (market-data, strategy, risk, planner, execution, gateway,
sim-exchange, nats, postgres, dash-api, dash-web, watchdog, notifier); daily logs
`/logs/<service>/YYYYMMDD.log`. Source: `deploy/chain/` (compose.yml, compose.local.yml,
algo, monitor.py moved from deploy/paper_trading, logging/ moved from deploy/live/logging).
`algo install <root>` creates the tree, copies chain files, writes chain.env/config
defaults and a 0600 secrets/chain.env once (never overwritten). Data uses bind mounts
(data/postgres, nats, market, dashboard/{watchdog,audit,alerts,notifier}, telemetry,
receiver). Receiver: chain paths, tag = container name, YYYYMMDD files, root must be
named `logs`. Dashboard Go accepts the short names besides the old ones (running VPS).
Trading economics, streams, DB names and service binaries are unchanged.

Local WSL acceptance (root ~/algotrading-runtime, images algotrading-runtime:chain,
algotrading-dash-api:chain, algotrading-dash-web:chain; build log storage/chain_build.log):
install twice (idempotent; secrets 0700/0600); compose config 15 services; `algo up`
started all 13 long-running services healthy/running and both one-shots exited 0;
every line of the 8 C++ service logs matches the agreed format (NATS/PostgreSQL/Go
dashboard keep their own formats); dashboard login, PAPER mode, host telemetry HEALTHY,
simulated exchange connected and 13 short container names; `down risk`/`up risk`,
`restart planner`; `show` masks all secrets (0 secret values in 697 lines); `backup`
wrote a 0600 archive with dashboard/market/nats/postgres and restarted all 13. Tests:
daily_service_logs_test 8 OK, host_monitor_test 3 OK (1 skip), dashboard Go tests
(CGO_ENABLED=0) all OK. Docker Desktop cannot reach the WSL-loopback receiver, so local
installs use LOG_MODE=local (json-file + monitor copy into the same files); the VPS uses
the systemd receiver. Not done: VPS install (needs authorization), comparison collector
and Kraken account monitor inside the chain, removal of deploy/paper_trading.
Stop locally with `~/algotrading-runtime/chain/algo down`.

## Readable code logs — implemented and WSL-accepted locally (2026-10-11)

User decision: all lib/, live_trading/ and research/ logs use
`YYYY-MM-DD HH:MM:SS.mmm [LEVEL] Class::function : message` in UTC; long IDs, hashes,
streams and unrounded values only in DEBUG; dashboards out of scope. Implemented:
new `lib/src/logging/logger.*` (no log4cpp layout/rolling appender; label from
`__PRETTY_FUNCTION__`; FormatDayKey/FormatUsd/FormatQuantity), `service_logging.h`,
`executionOrderStatusName`, rewritten messages in all services, lib utils and research
mains, canonical_replay status/result lines (summary JSON contract unchanged),
`live_trading/readable_log.py` and `research/replay.py` logging. binance_simulator now
links libalgolib for the logger. Help text, progress bars, interactive RealTest review
screens and report tables remain plain stdout.

WSL acceptance (`bash storage/logging_validation/run.sh`; evidence in
storage/logging_validation/, ignored by Git), final pass 2026-10-11:
- whole-project meson build: PASS (103 steps). The first attempt failed because fmt 8
  rejects `<fmt/format.h>` after `<fmt/core.h>`; logger.h now includes `<fmt/format.h>`.
- Step59: PASS, 107 days, 50 fills, RealTest 25/25, zero differences, fingerprint
  94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2 (unchanged).
- fast (20 days) and system (20 days) samples: PASS. Every line of runtime.log and of the
  logger lines in research.log matches the agreed format; the only other research.log lines
  are the documented plain report tables. 20-day system fingerprints differ from the
  107-day accepted one by design (different window).
- kraken_account_test (26) and daily_service_logs_test (8): PASS.
- validation/*audit*.sh: 37 audits, 32 PASS and the same 5 failures as the pre-change
  baseline (execution_state/strategy time-handler, historical_market_data_feeder meson,
  t19_realtest_gate, t21_strategy_canonical_frontier): stale historical paths, not logging.

Integration fixes found by the run (no trading-economics change): research/replay.py
now compiles `lib/src/logging/logger.cpp` and links `-lfmt` for the canonical runner;
the full-run fingerprint moved from INFO to DEBUG (C++ runtime.log and replay.py; it
remains in summary JSON and Step59 output; `ALGOTRADING_LOG_DEBUG=1` shows it in both);
replay.py honors that variable. Operational note: replay.py puts
deploy/historical_replay/.runtime_bundle/lib first in LD_LIBRARY_PATH, so a stale
ignored bundle shadowed the new libalgolib.so (`undefined symbol Logger::Write`);
regenerated with `bash deploy/historical_replay/build_runtime_bundle.sh`. Rebuild the
bundle after lib changes before using `replay.py fast`. Non-interactive samples need
`REALTEST_NONINTERACTIVE=1`.

Host receiver (user decision 2026-10-11, local source only): deploy/live/logging no longer
stores its own header. rsyslog emits an internal routing header (receipt date + service)
that clean_daily_logs.py consumes to pick the file; each stored line is exactly the
service's line (source_time/severity are not stored; the line carries event time and
level). Non-conforming sources (e.g. NATS/PostgreSQL stdout) now lose their receipt
timestamp in the file. daily_service_logs_test updated and passing. The VPS still runs the
previous receiver; installing this one needs a new explicit authorization. Existing VPS
files keep the old header. Not changed: INFO lines of the ready messages still show the
short stream name (e.g. `stream ALGOTRADING`) although the rule says streams belong in DEBUG.
Kraken: no demo; private integration will use explicitly authorized small real-money tests
(ROADMAP 5-6).

## Pending reporting requirements recorded (2026-10-10)

The user requested separate Commission and Funding columns in dashboard Trades,
with USD values and PnL including those costs/receipts; gross trade PnL is optional.
Actual event accounting, attribution and consistent net totals remain prerequisites;
missing costs must stay unavailable and commissions/funding must not be counted
twice. Future user-specified dashboard adjustments are a separate roadmap task.
The roadmap is consolidated to remaining work, preserving accepted local/VPS study
preparation and pending multi-day/private/recovery acceptance. This is a documentation
update only: no reporting/accounting implementation, deployment, restart or study
configuration change. Prior validation evidence below remains unchanged.

## Latest validated baseline

### VPS PAPER activity study and fast comparison deployed (2026-10-10)

The user authorized a VPS forward PAPER study with strict RSI(7) entry > 50 and
exit < 40, plus simulated-live versus fast-backtest results in its dashboard.
The separate activity profile preserves original 80/70 configuration and uses
separate NATS/PostgreSQL volumes; original account/message state is retained.
The comparison collector freezes fully applied/reconciled daily market/account
observations and invokes CURRENT TradingEngine through the existing fast research
binary. It warms indicators only, preserves daily candidate lists and persistent
signals, and observes execution-open prices without unfinished-close leakage.
PAPER close-based quantity versus fast next-open quantity differences remain visible;
cash/equity use common observed open marks. Funding/slippage are not modeled.
GET-only Kraken plan observations remain separate from simulated fills/private funds.

Two bounded WSL baseline tests pass: poisoned future close exclusion, next-open
quantity, commissions, deterministic rerun, date joins and signed exposure. Full
Go tests and frontend image/type build pass. Fresh Step59 PASS: 107 days, 50 fills,
RealTest 25/25, zero differences; all restart/dashboard/pacing paths retain fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/paper_activity_step59.log, paper_comparison_go.log and image logs.
The complete WSL build passes for eight service targets plus the fast research
binary. Three images were exported via docker save, transferred and loaded on the
VPS; no destination C++/frontend build was needed. The bounded PAPER fixture passes
completed-bar isolation, execution-open prices, actual fills/accounting, dashboard
telemetry, stale collector and restart. Initial local attempts found an older API
image and exhausted Docker subnets; selecting matching images and removing only
this turn's stopped fixture networks resolved them. Original local image tags and
all fixture volumes/evidence were preserved. Accepted fixture:
storage/paper_validation/algotrading-paper-7ec7043c76b3/.

VPS acceptance passes with thirteen running services and enabled/active trading,
telemetry and comparison units. Its first observed execution day is 2026-10-10:
two simulated positions (NEARUSDT/QNTUSDT), zero signal differences, PAPER equity
99980.00204792136 USD versus fast equity 99980 USD, and cash 79982.04996928117
versus 79980 USD. The NEAR close/open gap explains the retained quantity difference.
Collector restart preserves the single observed row and economic values, without
resubmitting simulated orders. Kraken public plan observation is retained; no
private Kraken credentials/account/submission was introduced on the VPS.
Authenticated API and one-page browser acceptance verify the 50/40 rules, chart
and comparison tables. The original PAPER account/message volumes remain selected
only by the old profile, with a stopped-state backup under
/opt/algotrading/storage/vps_deployment/activity-before-1791646615/.
Evidence: storage/vps_deployment/activity_acceptance.json and
dashboard_activity_comparison.png/.txt; the VPS retains frozen study input/fast
results under storage/paper_trading/comparison/ and public Kraken reports under
storage/kraken_shadow/algotrading-paper-activity-forward/.
Loopback dashboard remains port 8092; the local SSH tunnel exposes localhost:8093.
Credentials were consumed only internally and never displayed. Multi-day forward
acceptance, funding/collateral/private integration and broader VPS recovery remain
pending. No full-history or broad slow-browser campaign ran.

### Daily portfolio equity percentages; further order-limit work cancelled (2026-10-10)

The user cancelled additional order-limit/admission work and its pending clarification.
Preserve existing order planning, sizing and constraints. Warning-only account
monitoring remains implemented; actual local forward shadow is the next task.

Daily Telegram position lines now include marked absolute USD notional as a percentage
of that snapshot's net venue marginEquity. Yesterday retains yesterday's denominator;
existing historical snapshots are preserved. Missing marks and nonpositive equity
remain unavailable rather than producing invented percentages. Exposure may exceed
100%; this display is not funded margin or a new sizing constraint.

Five WSL portfolio tests and twelve offline monitor tests passed; the optional Docker
contract case was skipped in this targeted run. The local observer was restarted and
its healthy source/notifier retained two events and two receipts without duplicate
delivery. Daily decline remains unavailable until a preceding-day snapshot exists.
Evidence: storage/kraken_shadow/account-alerts/delivery-acceptance.json.
No credentials were displayed, no summary was forcibly resent and the VPS is unchanged.
This presentation-only change did not require another Step59 run; the accepted
fingerprint and previous bounded gate evidence below remain the baseline.

### User-selected warning-only account thresholds implemented locally (2026-10-10)

The user rejected automatic account stops/closures. USD 50 minimum net equity and
10% available USD remain warnings only. The local monitor now also warns when current
net equity is at least 10% below the preceding UTC day's last observed equity, or
gross confirmed open-position USD exposure exceeds 5x current net equity. Exactly
5x does not trigger the exposure warning. There is no independent daily-loss stop
or peak drawdown threshold. Equity comparisons include BTC valuation and transfers;
they are not a trading-only PnL calculation. Pending orders are not filled exposure.

The gross calculation reuses fresh verified public linear USD marks and sums
absolute LONG/SHORT notionals. Daily comparison reuses preserved UTC holdings/equity
snapshots. Missing prior-day history, missing/stale prices or nonpositive denominators
remain unavailable and cannot manufacture a resolution. status.json lists those
unavailableChecks independently of authenticated source freshness. Warning events
share the existing transaction/deduplication and Telegram lifecycle; no new sender,
orders, BTC sale, core Account mutation or stop command was introduced.

Thirteen WSL monitor tests passed including the bounded network-disabled Go contract
case. Five portfolio, 26 account, 13 public Kraken and five shadow tests passed.
New cases verify exact 10% daily boundaries without submission/admission mutation,
daily rather than peak comparison, missing-day retention, exact 5x versus >5x,
and missing-price retention plus diagnostics. The real local observer was restarted
with the finalized code. Its source/notifier are healthy and retain two events and
two receipts (previous reserve warning and today's daily summary), without duplicates.
Daily decline is currently unavailable because no preceding-day snapshot exists.
Evidence: storage/kraken_shadow/account-alerts/ and delivery-acceptance.json.

The user also requests an initial 10% new-order cap. Its total-notional-versus-funded-
margin basis is an essential pending clarification; no new admission cap was invented.
Source verification found existing equal-weight sizing of 10% per full signal and
a 1.5x hard gross strategy scaling constraint. These are preserved and are distinct
from the warning-only 5x actual account exposure. VPS/credentials/history remain
unchanged. Finish the order-basis clarification/guard before actual forward shadow.

Fresh bounded Step59 PASS: 107 days, 50 fills, RealTest 25/25, zero differences;
determinism, resumed/system/dashboard and paced paths converge to fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. No full-history or browser campaign ran.

### Compact Telegram messages and daily USD portfolio accepted locally (2026-10-10)

All shared Telegram formatting now shows importance, account/system, reason and
concise figures. CRITICAL is displayed as URGENT and recovery as INFO. Optional
account labels pass through the source/notification contracts; Kraken defaults to
Kraken. Hash IDs, notification IDs, technical event/correlation metadata and raw
source timestamps stay in durable evidence rather than the chat message. Existing
delivery receipt deduplication is unchanged; normal details are bounded at 600
characters and portfolios use the existing overall Telegram limit.

The read-only account monitor emits one INFO daily summary after the configured UTC
hour (default 09:00), transactionally persisted with its event. Gross USD wallet
balance and net venue margin equity are separate, explicit fields. Linear Multi-M
positions use fresh public mark prices for absolute USD exposure with LONG/SHORT
direction; missing/unsupported/stale prices remain unavailable. Yesterday uses the
last observed snapshot in the preceding UTC calendar day and reports its timestamp;
absent days are unavailable, never invented zero holdings. This is a holdings
comparison, not a complete intraday trading history. No account/strategy/execution
economics, order writes, BTC sales or stops were changed.

Five new portfolio tests, nine monitor tests including network-disabled Go contract
acceptance, 26 account tests, 13 Kraken public tests and five shadow tests passed.
Go go test ./... passed with CGO_ENABLED=0, matching the Docker build. The initial
default-CGO attempt lacked libpq in the test image; this was an environment failure.
The updated local image was built and both local Telegram notifier containers were
replaced while preserving their notification volumes. Three user-authorized,
clearly TEST-labelled format previews were accepted by Telegram; fixtures/receipts:
storage/telegram_previews/telegram-preview-1791641194340836467/.
The real account daily portfolio was also accepted: account-alerts deliveryCount=2
including the previously retained warning, with INFO admission and two durable
receipts. Source heartbeat was healthy; there was no replay of the old warning.
Actual account currently has no open positions; yesterday is unavailable because
daily holdings capture started today. Local monitor remains active; VPS unchanged.
All credential-file consumption was internal to programs/Docker, never displayed.

Fresh bounded Step59 passed: 107 days, 50 fills, RealTest 25/25, zero differences,
all deterministic/resume/dashboard/paced paths converge to fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Permanent observer installation/history
retention and remaining stop-control agreement/actual forward shadow remain pending.

### Local Kraken margin/reserve Telegram monitoring accepted (2026-10-10)

The read-only account monitor publishes durable step43-v1 warning/recovery events
for the existing Go notifier in an isolated local container. It polls every 30
seconds using the existing user-selected USD 50 net equity floor and 10% available
USD policy. Current/stressed USD shortages share one warning; equity-floor and
initial/maintenance margin warnings remain distinct. No order, automatic conversion,
BTC sale, entry stop or core Account update is performed. Failed/stale/invalid reads
cannot manufacture a recovery. An exclusive process lock and policy/account-bound
journal protect producer restart and source identity.

Nine WSL monitor tests passed, including network-disabled TEST_FILE Go warning,
resolution and notifier restart acceptance. Existing 26 account, 13 public Kraken
and five shadow tests also passed. A real authenticated local read produced one
USD_RESERVE warning, accepted by Telegram with one durable delivery receipt.
Repeated reads and observer restart retained one event/receipt.
One subsequent poll failed safely: the notifier paused on the failed heartbeat,
retained the active warning and resumed after a fresh successful observation without
another delivery. The sanitized failure does not establish its upstream cause.
Evidence is under ignored storage/kraken_shadow/account-alerts/: events.jsonl, status.json,
account-observations.sqlite, notifier.json and delivery-acceptance.json. No .env
contents were opened by assistant tools; credential consumption remained internal.

The local observer remains running in WSL with a separate Go notifier. It requires
the PC/WSL/Docker and network; there is no observer reboot installation or VPS change.
Permanent observation-history retention is pending. Remaining loss/connectivity/
reconciliation and entry-stop versus closing behavior precede actual forward shadow.

Fresh bounded Step59 passed on verified current library/canonical source: 107 days,
50 fills, RealTest 25/25, zero differences; all deterministic, resumed, dashboard
and paced paths converge to fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Generated .ai navigation was refreshed
and checked; credential and monitoring artifact paths remain ignored by Git.

### User account floor and relative USD reserve validated (2026-10-10)

The user chose a USD 50 minimum account value and at least 10% available USD.
The explicit implementation basis is reported marginEquity in USD (net usable
collateral/equity), never leveraged position notional. The ignored policy file
storage/kraken_shadow/account-policy.json specifies minimum_account_equity_usd=50,
minimum_usd_reserve_fraction=0.10 and zero additional assumed future cash debit.
Zero assumed debit is not zero trading fees/funding; future cost forecasts remain
pending. Fixed-USD illustrative policies remain supported.

The equity floor and current USD reserve are independent checks. Available USD is
the lesser of reported USD quantity and availability; the requirement is 10% of
positive reported equity. At USD 50 this is USD 5. Equity below the floor flags
ACCOUNT_EQUITY_BELOW_MINIMUM; insufficient current USD flags USD_RESERVE_LOW.
Loss/charge stress is separately labelled USD_RESERVE_LOW_AFTER_LOSS_STRESS,
without subtracting PnL twice from account equity. No allocation, conversion, BTC
sale, order admission or closing behavior is connected to these observations.

Twenty-six WSL account tests plus thirteen Kraken public and five shadow tests
passed. New checks cover boundaries, equity versus leveraged notional, missing USD,
zero/negative equity, invalid policies, separate stress warnings, policy-bound
journal isolation and CLI report export. Real read-only policy evaluation passed
in a separate preserved journal: storage/kraken_shadow/account-live-policy-50-10/
contains report.html, report.json, account-observations.sqlite and last_attempt.json.
The account passes the USD 50 net floor but reports both low-current-USD and
low-stressed-USD warnings, with zero positions/orders. An earlier snapshot attempt
failed safely before observation; the successful fresh read did not relax clocks.
Credentials were consumed internally only, without assistant-visible contents.

Fresh bounded Step59 PASS on verified current canonical source: 107 days, 50 fills,
RealTest 25/25, zero differences and deterministic/restart/dashboard/pacing convergence.
Fingerprint: 94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. VPS/Telegram/core trading remain unchanged.
Remaining loss/connectivity/reconciliation and entry-stop versus close behavior must
be agreed and tested before private orders. These are read-only warnings only.

### Kraken authenticated read-only account inspection accepted (2026-10-10)

The user saved the Futures keys and authorized the account check while requiring
that credential files never be opened/displayed by the assistant. The program
alone consumes storage/kraken_shadow/.env internally; no shell sourcing, environment
dump, credential output or assistant-visible file contents were used. The dedicated
loader accepts the two named variables, optional quotes, UTF-8 BOM/CRLF and comments;
duplicate/missing/invalid entries give constant corrective errors without values.

`python3 live_trading/kraken_account.py --account` now passes a real authenticated
inspection after the initial HTTP 403 was resolved by an explicit application
User-Agent. The permission gate accepted General READ_ONLY/transfer NO_ACCESS.
All five fixed GETs completed and the authoritative multiCollateralMarginAccount
wallet reports BTC/USD currencies, zero positions and zero open orders. This does
not prove a funded BTC allocation, future order acceptance or liquidation readiness.
The check remains REVIEW_REQUIRED solely for USD_RESERVE_POLICY_NOT_CONFIGURED;
no actual reserve/allocation was assumed and no illustrative policy was applied.
No private order, cancel, transfer, settlement change, BTC sale or core Account
update occurred. VPS/Telegram remain unchanged; unrelated working-tree work is preserved.

Safe evidence is retained under ignored storage/kraken_shadow/account-live/:
report.html, report.json, account-observations.sqlite and last_attempt.json.
Only normalized account observations and our status wording persist, never raw
permission replies, keys, signatures or raw account UID. Failed attempts cannot
produce accepted reports. HTTP 401/403 have distinct sanitized diagnostics; 403
alone does not establish invalid credentials. Fixture and actual journals remain separate.

Twenty WSL account tests plus thirteen public Kraken and five existing shadow tests
passed. Fresh bounded Step59 also passed on source-verified current canonical code:
107 days, 50 fills, RealTest 25/25, zero differences and restart/dashboard/pacing
convergence. Fingerprint:
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Gate evidence: storage/pipeline_risk_step59.log.
New coverage includes internal loader formats, CLI/source separation,
User-Agent, HTTP diagnostics and durable failed-attempt status. Actual account
inspection is read-only acceptance, not forward strategy or live-capital acceptance.
Next: agree on BTC/USD reserve and stop policy, then actual forward venue shadow;
funding/conversion/interest economics and supported private lifecycle testing remain pending.

### Kraken read-only account preparation accepted locally (2026-10-10)

The user deferred saving the Kraken key. `live_trading/kraken_account.py` prepares
a separate reader with five fixed GET routes: permissions, wallets, open positions,
open orders and PnL preferences. READ_ONLY/no-transfer permissions are checked
before account reads. Authentication was tested with synthetic in-memory credentials;
no actual keys were read, saved or used. Redirects, proxy inheritance, oversized
responses and stale/non-UTC clocks are rejected. GET authentication omits Kraken's
optional nonce, with no body/query. Errors/reports exclude keys and the raw account UID.

Only the reported Multi-M flex wallet is supported. Venue collateral, marginEquity
and initialMarginWithOrders are authoritative: no fixed haircut is reapplied and
PnL/funding are not added twice. A USD-reserve stress check includes assumed future
cash debits and unrealized losses. Missing USD/policy, margin shortfalls, other
collateral or non-USD settlement require review. Short quantities and partial orders
remain observations, never new fills. Separate SQLite state binds account/source/
environment/policy, rejects conflicting/out-of-order captures and deduplicates after
restart. It never updates lib/Account, submits orders, sells BTC or approves trading.

Fourteen offline WSL account tests, thirteen existing Kraken public tests and five
existing shadow tests passed. Synthetic evidence: storage/kraken_shadow/account-local-20261010/
contains fixture.json, policy.json, report.json, report.html and account-observations.sqlite.
Artifacts are ignored; balances/reserve thresholds are illustrative, not user inputs.
Fresh Step59 PASS on byte-verified canonical source: 107 days, 50 fills, RealTest
25/25, zero differences and restart/dashboard/pacing convergence; fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Gate evidence: storage/pipeline_risk_step59.log. VPS and Telegram remain unchanged.

Actual credential loading, account/product acceptance, a supported private test
environment, agreed reserve/stop policy, forward shadow and derivatives funding/
conversion/interest accounting remain pending. The CLI is explicitly fixture-only;
the account reader is separate from the runtime. Usage: live_trading/README.md.

### Kraken public shadow and mixed BTC/USD preparation accepted (2026-10-10)

The user selected Kraken Multi-M as the initial integration target, with BTC held
as collateral and USD for charges/losses. No account allocation or reserve threshold
was chosen. `live_trading/kraken_shadow.py` now reads fixed public GET instruments/
tickers only, normalizes PF_ linear USD contracts and observes immutable CURRENT
USD notional plans against current marks. Negative precision/scaled source assets,
inactive/inverse exclusion, unavailable books, incomplete metadata and fresh server
clocks are explicit. An empty book is retryable unavailability, not evidence of
an unsupported market. No credentials, submit/cancel routes, wallet transfers,
strategy recalculation, private execution or core-account economics were added.

ShadowJournal accepts a venue assessor with durable source/environment/schema
identity while preserving fixture defaults, source risk hashes, payload conflict
checks and retry/restart ordering. Fixture journals cannot be relabelled as Kraken.
Raw public bytes/GET receipts/hashes, normalized snapshots and source plans persist
per capture so retained observations remain linked to their original evidence.

The live public study passed: 201 PF_ catalogue entries, one exported PAPER fixture
plan, BTC/ETH observations on PF_XBTUSD/PF_ETHUSD, and a restart with exactly one
completed durable plan and two retained raw captures. These are observations of
historical fixture intents, not actual forward strategy or private order acceptance.
The illustrative cross-margin scenario (0.1 BTC, USD 2000, assumed USD 50 cash
charges, unrealized PnL -100 and funding -1, initial/maintenance margin 500/250,
USD reserve 500) passed scenario checks. These numbers are synthetic inputs, not
user balances, recommended allocations or verified venue margin requirements.
The diagnostic uses the BTC index and reviewed 1% haircut, distinguishes USD
reserve from collateral equity, flags uncovered losses/margin shortfalls and
never sells BTC or returns account trading approval. Actual margin schedules,
isolated/unified wallets, other assets and authoritative balances remain pending.

Thirteen focused Kraken WSL tests plus five existing shadow unit tests passed.
The full saved-response audit passed: raw hashes, GET routes, catalogue identity,
original plan/snapshot hashes, quantities/read-only state and scenario arithmetic.
Evidence: storage/kraken_shadow/local-20261010-btc-usd/{report.html,report.json,
observations.sqlite,raw/}; hypothetical input: storage/kraken_shadow/example-collateral.json.
All artifacts are ignored; no VPS or Telegram changes. Existing unrelated work was
preserved. Fresh WSL Step59 PASS on byte-verified current native canonical source:
107 days, 50 fills, RealTest 25/25, zero differences, deterministic/restart/dashboard/
pacing convergence; fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log.

The legacy demo public instruments probe failed on a forbidden redirect; no live
fallback occurred. Kraken's current [testing guide](https://support.kraken.com/articles/360024809011-api-testing-environment-derivatives)
also announces decommissioning of that environment; confirm an available supported
test environment before private lifecycle tests. The user confirmed their account
country is Spain and Futures is enabled (2026-10-10). This resolves the requested
country/activation clarification; wallet type, BTC eligibility on that wallet,
USD profit settlement and API access still require actual account evidence.
Next: validate the prepared read-only wallet/margin inputs with a Derivatives key
(General API Read Only; Withdrawal API No Access) and agree on reserve/stop policy
before private test integration. No private credentials or API checks are available
yet; user-reported activation is not an authenticated account acceptance.
BTC/USD can reduce conversions while USD covers charges/losses; trading fees,
funding and uncovered-loss interest remain economic requirements. Official EEA
collateral/charges/eligibility sources and usage are linked in live_trading/README.md.

### Public exchange coverage accepted (2026-10-10)

`tools/exchange_coverage.py` captured public market metadata and daily candles at
11:07-11:08 UTC. Binance server time bounded 25 completed UTC days: September 15
through October 9 inclusive. The reference is currently TRADING crypto USDT
perpetuals, not Binance spot or all quote currencies; 523 candidates were inspected.
Ranking sums actual candle quote turnover (field 7), never base units or close
price estimates. All top-50 assets had 25 observed candles. Current-listing
survivorship bias is explicit; this is current coverage, not a historical universe.

Kraken covers 38/50 (76%), Hyperliquid main covers 36/50 (72%), and Hyperliquid with
inspected HIP-3 markets covers 37/50 (74%). These assets represent respectively
95.6911%, 94.0169% and 95.2846% of Binance top-50 turnover, not venue liquidity.
Kraken covers all top 20 with Multi-M PF_ contracts; Hyperliquid main covers 19,
and HIP-3 xyz:QNT supplies the twentieth. MOVR/MUBARAK are Kraken-only and LIT is
Hyperliquid-only among the compared top 50. Active/nonexpired/nonsuspended metadata
and positive mark prices are required. Explicit scaled-contract aliases retain
1INCH and distinct NEIRO/NEIROETH; identity matches do not prove sizing equivalence.

Evidence: storage/exchange_coverage/20261010T110714Z/{comparison.json,coverage.csv,
report.html,raw/}. Raw responses retain retrieval time, endpoint/request and body
hash. Five focused WSL tests passed; the offline snapshot audit independently
recomputed every candidate's candles, complete ranking, live filters, aliases,
per-asset venue matches and all three counts. Generated evidence is ignored.
No trading source, credentials, private API, VPS deployment or Telegram sends changed;
the existing Step59 baseline below is retained without rerunning an economic gate
for this independent public-data study.

Public collateral rules were checked against current official documentation:
[Kraken Multi-M](https://support.kraken.com/articles/4843323030164-derivatives-collateral-currencies)
accepts BTC with 1% haircut and 0.20% conversion fee; uncovered USD losses can have
additional interest/conversion effects.
[Hyperliquid portfolio margin](https://hyperliquid.gitbook.io/hyperliquid-docs/trading/portfolio-margin)
lists BTC at 0.5 LTV, requiring master-account weighted volume above USD 5 million
or account value above USD 10000, with account value below USD 25 million and
supply/borrow caps. This is distinct from ordinary USDC margin and borrowing has
costs. Public rules do not establish this user's jurisdiction/account eligibility.
Kraken remains the technical preference from coverage and the user's BTC/account
requirements; confirm product/account eligibility and collateral economics before
venue selection, real read-only integration and forward shadow acceptance.

### Local shadow-trading preparation accepted (2026-10-09)

The one-command WSL `python3 validation/shadow_trading_test.py` study passed with
current packaged runtime/API/web images and isolated PAPER state. The existing
service chain produced 200 completed BTC/ETH bars, exactly two BUY intents of USD
10000 at close(T)=199 and two simulated next-open fills at 200. Worker restart
retained fills, cash/positions and the persisted risk evaluation. The shadow hook
read the actual planner checkpoint and retained its request/plan without changing
the original durable plan. No new strategy, planner or fill simulator was introduced.

`live_trading/shadow_observer.py` owns only read-only observation and its separate
SQLite journal. It binds the source risk-configuration fingerprint and immutable
message/payload hashes, rejects conflicts/out-of-order input and reuses already
completed observations after duplicate delivery/restart. Failed fixture reads remain
retryable without becoming successful empty data or unsupported-market skips.
The reader allows only numeric loopback HTTP GET /snapshot, disables proxies and
rejects redirects; it has no private submission/cancellation interface or credentials.

The integrated fixture passed nine grouped checks: real producer quantities use
close(T), duplicate/observer restart, unsupported perpetual skip, HTTP/disconnect/
redirect recovery, stale/future/incomplete/missing metadata, minimum quantity,
quiet/cancel intents, identity/configuration conflict and the HTTP spy. It retained
12 scenario plans and seven unavailable attempts; 19 HTTP calls were all GET
/snapshot, with zero private writes. Synthetic error-case rows are explicitly
labelled and are not additional strategy decisions. Five focused WSL regression
tests also passed, including malformed top-level metadata, failed-attempt identity
conflict, future decision/side mismatch and out-of-order rejection before HTTP.
The final observer was rechecked with the actual saved producer export in fresh
fixture state after the final error-guard/report changes.

Evidence: storage/paper_validation/algotrading-paper-958436ce42ee/accepted.json and
shadow/{producer-input.json,accepted.json,report.json,report.html,venue-calls.json,
observations.sqlite}; shadow/current-source-recheck/ retains the final component run.
Reports and disposable credentials are ignored. Owned containers/networks were
removed; volumes/evidence were preserved and the normal Telegram notifier remained
running. No VPS changes, private venue calls, real Telegram sends or full-history/
browser campaign. Source trading behavior is unchanged.

Fresh required WSL Step59 PASS: 107 days, 50 fills, RealTest 25/25, zero differences,
continuous/resumed/system/dashboard/pacing convergence and fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log, using byte-verified current native source.

This accepts local fixture preparation, not actual selected-venue shadow trading.
The risk fingerprint is not a complete strategy/market/venue configuration manifest.
Full aligned backtest comparison, real venue mapping/rules/price freshness, fees,
funding, FX/BTC collateral/account eligibility and private order/fill/reconciliation
acceptance remain pending. Reports leave costs, fills and PnL unavailable rather
than inventing them. Next prerequisite: reproducible venue coverage/collateral
analysis and selection before actual selected-venue forward observation.

### Mobile-access cancellation and next local task (2026-10-09)

The user cancelled mobile dashboard access and removed it from the roadmap.
No phone login acceptance, domain, VPN or public web deployment is pending.
The unused local bridge/tests and their past validation evidence are preserved;
they are not active operations or prerequisites for trading work. No VPS change
or network exposure was made for this cancellation.

Next is preparing bounded local shadow-trading tests. The roadmap now defines
the no-private-submit/cancel boundary, isolated virtual state, selected-venue
read-only evidence, unsupported-market skips, aligned baseline comparison and
disconnect/duplicate/restart criteria. This is an acceptance plan, not a completed
harness or venue run. Source inspection confirms the existing gateway has backend
and Hyperliquid dry-run modes; the latter logs raw quantities with
venue_rules_applied=false and submitted=false. PAPER still uses a simulated backend.
Coverage, BTC-collateral/account checks and venue selection remain prerequisites
for actual selected-venue acceptance. Existing economic/replay evidence is unchanged.

### Historical mobile-access preparation, subsequently cancelled (2026-10-09)

`deploy/paper_trading/mobile_access.py` provides an opt-in TLS 1.2+ bridge from one
exact phone IPv4 address to an already-running loopback dashboard or VPS SSH tunnel.
It refuses public/wildcard listener addresses, checks certificate validity/IP,
streams bytes without whole-response buffering and does not log requests/secrets.
Generated certificates/keys are excluded under `storage/mobile_access/`.
The source API still owns authentication, role checks, CSRF and session revocation.

WSL validation passed six bounded transport checks plus actual Go API authentication
through TLS: unauthenticated 401, viewer login, Overview/Pipeline/Risk/Infrastructure
HTTP 200, viewer manual-preview rejection, missing-CSRF logout rejection, successful
logout and revoked-session 401. The isolated packaged API used embedded mock data
and disposable fixture credentials; no trading workers or Telegram sends were used.
Its container was removed. Evidence: storage/mobile_access/api-validation/test.log
and accepted.json. This tests real API authentication, not real market-data readiness.
All six transport tests also passed on Windows with the installed Git OpenSSL;
the optional Go-fixture test was skipped there. Python CLI help and whitespace checks
passed. No full-history or browser campaign was rerun.

Mobile setup/runbook was prepared; actual handset acceptance was not performed
and the user subsequently cancelled this requirement. The observed PC Wi-Fi
IPv4 is 192.168.1.33. No mobile listener/firewall rule or SSH tunnel was left running;
normal services and the VPS were preserved. Remote/mobile-data access is separate.

Fresh WSL Step59 PASS using the byte-verified current native trading source: 107
days, 50 fills, RealTest 25/25, zero differences, continuous/resumed/dashboard/paced
equivalence and fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Trading source/economics unchanged.

### Local PAPER backup, restoration and connectivity acceptance (2026-10-08)

The opt-in PAPER_RECOVERY_TEST study passed using the current packaged runtime/API/
web images and isolated BTC/ETH public-data fixture. HTTP source loss failed ingestion
without committing bars/fills. NATS loss retained 200 already committed completed
bars but generated no fills until publication recovered, matching commit-before-
notification source ordering. Two actual simulated fills completed with all four
dashboard services stopped; cash/positions agreed with the simulated backend.

PostgreSQL outage produced explicit HTTP 503 from Risk rather than successful empty
data. After controlled worker restart and database recovery, retained fills, cash,
positions, processed fill IDs, next order/fill IDs and economic dates were unchanged.
This is explicit restart recovery, not unattended reconnect acceptance.

With all writers/broker stopped and the exchange outbox drained, a PostgreSQL custom
logical dump plus six stopped-volume archives were created. Checksums were verified
before restore; a deliberately wrong checksum was rejected. A separate fresh-volume
project restored identical contents for all 15 public SQL tables and every archived
file before workers ran. Market SQLite includes the stopped database and WAL/SHM;
NATS and watchdog/notifier/audit/acknowledgement stores are included. Manual-audit and
acknowledgement stores were empty in this fixture; their operator event lifecycles
are not accepted by copying empty volumes. Credentials remained separate/ignored.

Restored ingestion completed successfully, all trading workers stayed running and
same-day replay retained exactly two fills, cash 79879.39698492462, BTC/ETH quantities
50.25125628140704 each, next order/fill IDs 3 and zero unpublished exchange outbox
rows. Authenticated restored Risk preserved its configuration fingerprint/evaluations.
No duplicate orders/fills or accounting drift were observed in the bounded study.

Evidence: storage/paper_validation/algotrading-paper-5c503da259d6/recovery-accepted.json,
backup/manifest.json, baseline accepted.json and storage/paper_recovery.log.
Original/restored test containers/networks were removed; their data volumes and
backup evidence remain ignored. The existing normal local Telegram notifier was
preserved. VPS unchanged; no Telegram sends, private orders, full-history replay or
browser campaign. The backup is a quiesced maintenance snapshot with matching
images/settings, not online cross-store backup or off-host/retention/VPS acceptance.
The then-next mobile-access task was cancelled on 2026-10-09; see the latest priority above.

Fresh WSL Step59 PASS: 107 days, 50 fills, RealTest 25/25, zero differences and
fingerprint 94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
including rerun/restart/dashboard/pacing checks. Evidence: storage/pipeline_risk_step59.log.
Python syntax and Git whitespace checks PASS. Trading source/economics unchanged.

### Authenticated local dashboard browser acceptance (2026-10-08)

A bounded isolated PAPER study passed with 200 completed BTC/ETH bars, two
simulated fills, consistent account cash/positions and restart without duplicate
fills. Current packaged runtime/API and a newly built API-mode frontend were used.
The fixture forces TEST_FILE notifications and clears Telegram credentials; its
temporary viewer credentials and all generated evidence are ignored by Git.

Windows Edge acceptance passed for all eleven authenticated pages, invalid/valid
login, sidebar navigation, full document navigation, all six Overview filters with
explicitly unavailable real equity history, Pipeline expansion/Why/search,
persisted Risk rows/configuration provenance, stale Infrastructure telemetry,
viewer-disabled manual upload and HTTP 403 mutation rejection. Browser-only
blocking of the Risk resource showed an error/Retry boundary; unblocking and Retry
restored the report. Logout protected subsequent page access. No page produced a
captured browser runtime error during normal acceptance. This does not accept
operator routing, actual backend connectivity faults, phone access or VPS behavior.

The browser test exposed accumulated SSE connections during repeated full document
navigation: later REST requests timed out without reaching the API. The stream hook
now closes EventSource on pagehide and reconnects on pageshow, as well as closing
on React unmount. The same eleven-page navigation sequence passed after this change.
No trading economics, API data contracts or private routing changed.

Evidence: storage/paper_validation/algotrading-paper-a5d64a25c8dd/accepted.json,
browser/accepted.json and page screenshots; build output is
storage/dashboard_browser_build.log. Only this task's fixture containers/networks
were removed; their data volumes/evidence remain. The normal local Telegram notifier
is still running/waiting for a fresh watchdog; trading services remain stopped.
VPS unchanged. Next: isolated backup/restore/connectivity acceptance, including
continued trading while the dashboard is stopped, then mobile browser access.

Fresh Step59 under WSL PASS: 107 days, 50 fills, RealTest 25/25, zero differences,
deterministic rerun, system/dashboard restart and paced-speed invariance, fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Frontend TypeScript/Vite build, Python
syntax checks, Git whitespace check and dashboard source audit (33 markers) PASS.
The generated AI index was refreshed. No full-history replay, VPS change or commit.

### Historical comparison review accepted by the user (2026-10-08)

The user considers the four full-history differences explained and closes their
review: final-position closing conventions and intermediate RealTest RSI rounding.
This is user acceptance of the recorded differences, not a new exact-parity proof.
The original comparison CSVs, four DIFFERENT rows and full-history fingerprint are
unchanged. No whitelist or trading/economic adjustment was introduced, and no
full-history replay was rerun. Dashboard acceptance is recorded above.

### Continuous local Telegram delivery and recovery (2026-10-08)

The packaged notifier passed a real, explicitly authorized two-message controlled
test: a synthetic WARN OPENED alert and its RESOLVED transition, clearly labelled
TEST ONLY. Its source was an isolated watchdog-format journal, not a trading failure.
Disconnecting its Docker network left the event unprocessed with no delivery;
reconnection delivered it. Two restarts retained delivery decisions/receipts without
resending. An expired source heartbeat blocked consumption; fresh source recovery
resumed polling. Exactly two receipts remained. The test container and its own state
volume were removed; source/evidence remain ignored under
storage/telegram_validation/algotrading-telegram-6a374aeb866e/.

The notifier now requires a successful watchdog heartbeat within 90 seconds,
rejects timestamps over 30 seconds ahead and waits after a failed watchdog sweep.
This avoids replaying obsolete alerts while the local stack is stopped. PAPER
healthchecks reject a recorded lastError rather than treating an old success as
current health. Full Go suite and current API/notifier image build PASS; focused
tests cover healthy/stale/future/missing/error source states.

The normal local dashboard-alert-notifier is running with restart=unless-stopped
and the validated algotrading-telegram-api image, selected by ignored
storage/paper_trading/telegram.override.yml. All trading services remain stopped;
the notifier therefore waits for a fresh watchdog, not trading signals. Its state
and Telegram receipts use the normal durable PAPER volume, separate from TEST_FILE.
Evidence includes storage/telegram_continuous_go.log,
storage/telegram_continuous_build.log and
storage/telegram_validation/continuous_running.json. VPS unchanged; no real orders.
The current notifier healthcheck correctly reports unhealthy while its watchdog
source is absent; process running is separate from delivery readiness. Dashboard
contract audit PASS (33 markers), source clock audit PASS and Step59 PASS with
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills and RealTest 25/25 with zero differences. Gate evidence is
storage/pipeline_risk_step59.log. No full-history or browser campaign was rerun.

### Real Telegram setup acceptance (2026-10-08)

The user configured the local bot and sent a fresh private message. Its private chat
was resolved through the bot API and saved only in ignored storage/paper_trading/.env;
the configured chat was verified before the explicit packaged --test-message command.
Telegram accepted the setup message and the notifier persisted its durable receipt.
An initial sanitized transport failure was followed by a successful retry after
checking public HTTPS access and the runtime CA bundle. No credentials or private
message contents were printed. API acceptance is observed and the user confirmed
that the setup message appeared on their phone. Continuous watchdog-event delivery
is separate from this confirmed setup test.

Evidence: ignored storage/telegram_validation/real_send/accepted.json and send.log.
Continuous alert delivery was not started, no trading stack was started, and the VPS
was not modified. Actual watchdog-event delivery/recovery remains separate acceptance;
the four historical replay differences remain pending. No code/economic change or
release-gate rerun was needed for this credential/configuration setup.

### Local Telegram preparation and operational alerts (2026-10-08)

PAPER no longer hardcodes TEST_FILE: Telegram is an explicit sink setting, with
token/chat ID forwarded only to the notifier and TEST_FILE preserved as default.
The notifier supports --test-message for one manually invoked setup message, without
replaying watchdog alerts or changing trading state. The existing sink now respects
Telegram retry_after across all alerts while running; rejected events remain
unprocessed and retry after the deadline. Confirmed delivery receipts persist and
prevent replay after a missing notifier decision checkpoint. The in-memory throttle
resets on restart; a crash after remote acceptance but before local receipt write
can still duplicate delivery. No exactly-once remote-delivery claim is made.

Fresh host resource pressure, observed missing/stopped trading executables,
unsynchronized NTP and degraded daily PAPER progress now enter the existing alert
projection/watchdog lifecycle. UNKNOWN/stale telemetry is not a measured process or
clock failure; no-order daily success is not an outage. Existing notification
severity/cooldown/escalation/resolution policies are preserved.

WSL full Go suite PASS, including simulated rate limiting, later retry, cross-alert
deferral, durable receipt recovery, operational alert derivation and recovery;
current API/notifier Docker image build PASS. Isolated Compose rendering PASS for
default TEST_FILE and opt-in Telegram credential forwarding using fixture values.
Packaged --test-message rejects TEST_FILE with networking disabled. Evidence is
ignored storage/telegram_go.log, storage/telegram_api_build.log and
storage/telegram_compose.log. No real Telegram message was sent: user bot/chat
settings and real delivery remain pending. No VPS change, full-history run or commit.
The four historical replay differences remain a separate pending review.
Dashboard contract audit PASS (33 markers), no-legacy-clock source audit PASS;
Step59 PASS with the expected fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25 and zero differences across restart/dashboard/
pacing paths. Fresh gate evidence is storage/pipeline_risk_step59.log.

### Local runtime-owned Risk evaluations (2026-10-08)

PortfolioRisk optionally captures actual sized weights, asset/gross cap reductions,
gross scaling, annualized volatility diagnostics and rebalance actions. The service
stores a schema-versioned report with cycle/configuration identity atomically in
the existing decision checkpoint. Control messages and publish/checkpoint/ACK order
are unchanged. Runtime retries reject changed configuration identities. Risk shows
the persisted intermediates, reduction reasons, held quantity and configuration
SHA-256. HOLD preserves quantity and does not emit a newly evaluated target.

Missing sizing or volatility is explicitly unavailable; EqualWeight volatility is
not applicable. Optional volatility diagnostic failure does not fail valid trading.
Old schemas/rows and canonical replay snapshots retain unavailable evaluations.
Whole-account/venue margin breaches, marked real PnL and control readiness remain
pending; per-strategy target caps do not prove held exposure satisfies those limits.

WSL current-source native build PASS; focused C++ signed/capped/volatility/HOLD tests
PASS; full Go suite and API/frontend TypeScript/Vite Docker builds PASS. Authenticated
PostgreSQL/HTTP fixture PASS for old schema, same-cycle reports, rejected mismatched
reports and unchanged cash. Actual bounded eight-service PAPER fixture PASS: two
simulated fills, correct Risk evaluation, identical reports/fills/accounting after
restart. Test project algotrading-paper-c3470b99bc3b retained ignored evidence.
Step59 PASS: fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25, zero differences. Evidence is ignored
storage/risk_evaluations_*.log and storage/pipeline_risk_step59.log.
The first fixture startup hit exhausted local Docker network pools; only two
verified empty networks belonging to a stopped earlier fixture were removed before
retry. Its data volumes and other local projects were preserved. No full-history
replay, slow browser campaign, VPS change or commit was performed.

### Local Infrastructure observations (2026-10-08)

Infrastructure now separates container state from externally observed trading
executables. The host collector uses bounded Docker top queries with PID/name/state,
never process arguments, and distinguishes running, stopped/defunct, missing and
unavailable executable observations. Services retain Ready=false; presence does not
establish application responsiveness, an in-loop heartbeat or private readiness.
Stale snapshots discard process and clock observations with host/container metrics.

Daily PAPER progress observes persisted last_bar_close_timestamp and
last_execution_timestamp: yesterday's completed-day decision and today's applied
plan, including empty/no-order plans. UTC rollover has an explicit 30-minute grace.
Recent updated_at/fills cannot conceal stalled dates; other business-clock modes
are not compared against today's date. Plan application is not fill/reconciliation
acceptance. The UI labels host uptime, process state and container observations
explicitly, with more readable operational text.

Host NTP synchronization is read from systemd NTPSynchronized, independently of the
dashboard container. SYNCED/UNSYNCED require observed evidence; unsupported/unavailable
queries remain UNKNOWN and offset is unmeasured. Actual local WSL query returned
unavailable, so synchronized-host/VPS acceptance is still pending. Shell warnings
and global clock requirements reflect observed evidence without enabling routing.

Validation: WSL collector unit tests and an isolated real Docker process fixture
PASS (running, stopped inside a running container, absent, stopped container);
full Go suite PASS; final API/frontend Docker builds and TypeScript/Vite PASS.
Packaged authenticated PostgreSQL/HTTP fixture PASS for current no-order progress,
missing process, unsynchronized clock, stalled business dates and stale telemetry;
account cash remained unchanged. Step59 PASS: expected compact fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25, zero differences. Source audit and AI index PASS.
Evidence includes ignored storage/infrastructure_go.log,
storage/infrastructure_sql_http.log and storage/pipeline_risk_step59.log.
No full-history replay/browser campaign, VPS deployment, commit or trading-engine
change was performed. Test containers were removed; production PAPER remains unchanged.

### Local Pipeline/Risk read-model repair (2026-10-08)

Source tracing identified two empty-screen causes: service HOLD/no-new-target
instructions omit assets from DecisionBatch, and the simulation provider returned
empty Pipeline rows and risk-limit lists by construction. The real pipeline now
includes observed planner reference assets, explains truly empty asset lineage,
and distinguishes missing planner/first-decision checkpoints from no-order success.
Absent signals are UNKNOWN; absent targets remain HOLD rather than displayed zero
liquidation instructions. Database failures and mismatched timestamps remain errors.

Real Risk now reads allocation, sizer settings, gross/asset caps and rebalance policy
from the persisted service configuration identity. These are settings, not evaluated
breach/clipping evidence. Empty emitted decisions retain account cash and an explicit
no-new-target explanation. Combined approved asset weights use aggregated notionals
and emitted capital, fixing the addition of differently allocated strategy percentages.
Replay Pipeline now shows market/position/retained-order observations with expandable
stages and Why traces; cumulative earlier fills do not become current-cycle proof.
Replay Risk shows actual marked account positions without an invented armed kill
switch, policy limit, strategy signal or breach-free verdict.

The WSL Go suite passed, including quiet/HOLD, explicit FLAT versus absent signal,
missing planner, missing decision versus database failure, policy parsing,
multi-strategy capital weighting and replay observation cases. The packaged real API
passed an isolated local PostgreSQL/HTTP fixture: persisted policy identity, quiet
cycle, missing planner pending, mismatched account timestamp rejected, first-decision
pending and unchanged account cash. Its own containers/anonymous volumes were cleaned.
A bounded authenticated Edge check against a copied existing canonical replay
snapshot passed Pipeline rows, expanded stages/Why and Risk account/position rendering.
This reused retained replay evidence; no full-history study was rerun.

Step59 passed in WSL against a byte-verified native copy of current supported trading
source, with the unchanged compact fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`.
TypeScript/Vite builds passed. Evidence is under ignored `storage/pipeline_risk_*`;
the authenticated replay preview was subsequently stopped and removed at the user's
request. The VPS was not contacted or modified.

The later local Risk evaluation work above supersedes the intermediate sizing,
volatility and configuration-persistence gaps recorded at this repair. Whole-account/
venue breaches and current REAL marked exposure/PnL remain unavailable. This repair
alone does not establish full risk telemetry or private/live readiness.

### Local Overview charts and date filters (2026-10-08)

Implemented numeric USD equity/reference charts with dated axes, hover values,
independent X/Y zoom, two-axis pointer drag and reset. Working 1D/7D/30D/90D/1Y/ALL
filters select source observations by UTC dates, anchored at each series' latest
date. ALL shows all received history; 1Y is 365 days. A single daily observation
renders as a point. Filters affect the charts only, not current account snapshots.
Real/PAPER missing marked-equity history remains unavailable.

The WSL Docker TypeScript/Vite build and focused date-window tests passed:
all six ranges, sparse dates, compact canonical date labels, invalid/nonfinite
rows, and empty/single-point histories. A bounded local Edge browser check passed
all six buttons, hover USD readings, independent axis zoom, real pointer drag,
reset and navigation reset after switching timeframes. It used frontend-local
mock fixtures, not production trading evidence. Evidence is in ignored
`storage/overview_build.log`, `storage/overview_windows_test.log` and
`storage/overview_browser/overview.png`. A loopback-only demo preview runs at
`http://localhost:8094`; it is separate from PAPER and the VPS tunnel on 8093.

The VPS was not contacted or changed. No trading/API/accounting behavior changed;
Step59 was not rerun for this frontend-only work. The existing accepted economic
fingerprint remains the baseline. Pipeline/Risk is the next local roadmap task.

### PAPER quote-turnover migration accepted (2026-10-08)

The VPS PAPER deployment now uses `pure_rsi_quote_volume.json`: top 20 by the
25-completed-day average of Binance's actual USDT quote turnover within the
current 24-hour top-50 candidate set. Base-unit volume remains available; neither
strategy nor dashboard approximates turnover with volume multiplied by close.
The original raw-volume canonical profile remains unchanged.

A checked cutover backed up PostgreSQL, market SQLite and configuration before
changing only the strategy's liquidity source. The completed 2026-10-07
strategy/risk/planner checkpoints were preserved. The first new-metric decision
uses the completed 2026-10-08 candle, available on 2026-10-09 UTC. No completed
cycle was rerun, and no account, order, fill or consumer state was reset.

Post-deployment verification independently compared dashboard top-20 ordering
with SQLite quote-volume averages. The leading assets were BTCUSDT, ETHUSDT,
ZECUSDT, SOLUSDT and XRPUSDT; BTC averaged 11.792 billion USDT/day and ETH 8.844
billion. All 13 services were running, with 100,000 virtual cash, zero fills and
no pending backend outbox. The dashboard labels `SMA Quote Volume 25 (USDT)`;
old-cycle signals remain explicitly unaligned until the new decision. The rolling
comparison excludes old-metric cycles and correctly reports INSUFFICIENT_DATA.

Validation passed in WSL: quote-volume C++ tests, the Go suite, dashboard build
and Step59. VPS Step59 also passed with the unchanged compact fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`.
The actual packaged VPS fixture `algotrading-paper-2eb7a44cfb1f` passed completed
quote-data/future-candle isolation, two simulated fills with reconciled accounting,
dashboard telemetry/staleness and restart without duplicate fills. Its separate
containers were stopped; production trading state was not used for the study.

Packaging validation caught stale incremental C++ objects after archive extraction
had assigned zero timestamps. All eight runtime targets were rebuilt from a clean
build before acceptance, and source archives now preserve a current snapshot
timestamp. The fixture's optional cached data-server binary requires identical
source and an ELF header; actual trading services use newly built images.

Evidence lives in ignored `storage/vps_deployment/quote_*` files and the fixture's
`storage/paper_validation/` directory. Backups and previous image tags are retained.
This acceptance does not establish a historical quote-volume performance baseline,
private-exchange readiness or sustained VPS resource capacity. Two-day, 50-MiB
daily log retention remains configured; actual UTC rollover is still pending.

### Lightsail current-data PAPER deployment (2026-10-08)

Deployed the current uncommitted source snapshot to `/opt/algotrading` on the
user's Paris Lightsail VPS: x86_64, Ubuntu 24.04.4 LTS, 2 vCPU, 1,906 MiB RAM
and a 57.1-GiB root filesystem. Added 2 GiB swap as a transient reserve. No local
credentials, mutable databases, Docker volumes or research collections were copied.
The normalized SHA-256 transfer manifest verified 773 initial source/input files;
The final synchronized manifest verifies 774 files, with the installed PAPER/logging
units and AppArmor snippet matching source after the deployment fixes.
The original canonical CSV, RealTest reference and symbol map are included.

All eight PAPER runtime targets built on the VPS and passed container `--help`
packaging checks. The configured project has 20 targets; this deployment did not
build every research target. WSL and VPS Step59 both passed the compact 107-day
gate: 25/25 matched trades, zero differences, 50 canonical fills and fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`, including
determinism, checkpoint/resume, dashboard equality and pacing. Toolchains differ:
local GCC 11.4 / Arrow 23 versus destination GCC 13 / Arrow 25; the accepted bounded
outputs agree, not the compiler or binary hashes.

The real public-data deployment has 13 running services, 4,932 completed bars for
50 coins through 2026-10-07, aligned strategy/risk/planner checkpoints, no fills,
100,000 virtual cash, empty positions and no pending backend outbox. Host reboot
restores durable state and services; receiver restart preserves the same 13 container
IDs/start times. The API and a short authenticated browser check passed. Sampled
host RAM was roughly 640-725 MiB, and disk about 9.6 / 57.1 GiB with 47.5 GiB free
after retaining the fixture/build images;
these samples do not establish sustained capacity, peaks or complete-backtest usage.
The host reports synchronized NTP, while dashboard clock measurement remains UNKNOWN.
Existing global private-readiness/reconciliation badges remain unverified/pending.

Deployment fixes preserve trading logic:

- A VPS overlay gives NATS/PostgreSQL/API restart policies, and a systemd unit
  starts PAPER after Docker and the loopback log receiver.
- Scoped AppArmor rules permit the dedicated receiver and bounded Python writer;
  confinement stays enabled. Listener readiness is checked before dependent startup.
- PAPER wants the receiver rather than depending on its continued lifecycle, so
  restarting diagnostics does not stop trading.
- Dashboard API/watchdog mount market SQLite read-only. Writable observer mounts
  had created WAL/SHM as UID 70 and blocked UID 10001 ingestion after reboot.
  Existing auxiliary-file ownership is restored by the volume initializer, and
  read-only observers cannot recreate the conflicting writable files.

The two-day/50-MiB daily receiver and minute cleanup timer are installed/enabled.
Fourteen service/init identities have emitted daily files; the quiet volume
initializer need not emit one. Unique markers survive receiver restart, cleanup
runs successfully and all observed files remain below the cap. Real UTC rollover,
actual production trimming, prolonged outage and multi-day growth remain pending.
Eight local daily-log tests passed. The final isolated PAPER fixture passed on the
VPS (`algotrading-paper-ec80b2f4b53b`): completed-bar visibility, execution-only
opening prices, two fills with reconciled accounting, dashboard telemetry/staleness
and restart without duplicate fills. It uses separate state and stopped its own
containers. A local rerun could not allocate networks because Docker's default
address pools were exhausted; no unrelated networks/volumes were removed.

Evidence is under ignored `storage/vps_deployment/` locally and on the VPS, plus
the VPS fixture's `storage/paper_validation/algotrading-paper-ec80b2f4b53b/`.
Viewer credentials are fresh on the VPS; the ignored local viewer-only copy is
`storage/vps_deployment/.env`. The private key stays outside the repository.
Remote viewing uses an SSH tunnel on localhost:8093; local PAPER remains on 8092.
The VPS runs independently of the PC, while remote viewing needs the client tunnel.
No private venue, real funds or Telegram delivery is enabled. Next observe successive
daily cycles/UTC log rollover, then perform the remaining recovery/capacity campaign.

### Current public-data paper session running (2026-10-08)

Started the prepared `algotrading-paper` project locally with its separate durable
volumes and left all 13 long-running services plus the host/log collector running.
The real public Binance download produced 4,932 completed rows for 50 coins through
2026-10-07; available history begins 2026-06-30. Strategy, risk and planner each have
the aligned 20261007 checkpoint. Execution completed the 20261008 no-order cycle:
zero active signals/fills, 100,000 virtual cash, empty positions and no pending
backend outbox. Execution/backend cash and positions agree. This is an actual
current-data no-order cycle, distinct from the earlier two-fill synthetic fixture.

Authenticated Infrastructure API and a short headless Edge check passed: PAPER /
VIRTUAL FUNDS, CPU/RAM/disk amounts, fresh observation timestamp and service rows
render. All 13 containers are RUNNING; containers without Docker health checks
report WARN rather than claiming business readiness. Disk WARN reflects the local
drive (about 91% used, 41.5 GiB free), not a VPS measurement. A single container
snapshot totaled 167.37 MiB RAM and 5.65% CPU of one core; these are sampled values,
not sustained averages/peaks or a VPS capacity acceptance. Host totals separately
include WSL activity. Existing global live-readiness/reconciliation badges still
show unverified/pending evidence; this session does not accept private live routing.

Ignored evidence: `storage/paper_trading/current_data_acceptance.json`,
`startup_diagnostics.log`, `dashboard_infrastructure.png` and
`dashboard_infrastructure_text.txt`. The background monitor PID is stored in
`storage/paper_trading/monitor.pid`; the generated local stop helper checks process
identity and preserves volumes:

```powershell
wsl --exec python3 storage/paper_trading/stop_local.py
```

Open http://localhost:8092 as `viewer` with the password from ignored
`storage/paper_trading/.env`. Keep Docker/WSL and the PC awake for daily operation.
No trading source/configuration was changed or release gate rerun for this startup;
the accepted Step59 evidence below remains current. VPS creation, deployment,
multi-day operation and deferred fault/retention acceptance remain pending.

### Current-data paper preparation and dashboard telemetry (2026-10-08)

The separate `deploy/paper_trading` stack builds under WSL with current ingestion,
100-day PureRSI warmup, virtual cash, the existing simulated backend and real-source
dashboard. No VPS deployment or private exchange integration was performed.
The bounded fixture `validation/paper_trading_test.py` passed in project
`algotrading-paper-4d6e89bb5e20`; retained evidence is
`storage/paper_validation/algotrading-paper-4d6e89bb5e20/accepted.json` and
`storage/paper_integration_second.log`. It verified:

- Missing opening prices fail before completed-bar commit.
- Exactly 200 completed rows through 2026-10-07; poisoned unfinished current-day
  high/low/close/volume stay out of canonical SQLite.
- Two fills on 2026-10-08 at the supplied opening price 200; fees, virtual cash
  79,879.39698492462 and both positions reconcile between execution/backend.
- One aligned checkpoint per strategy/risk/planner; restart leaves fills, cash,
  positions and next fill ID unchanged, with no pending backend outbox.
- Authenticated dashboard Infrastructure returns actual host CPU/RAM/disk and 13
  project container observations; 40-second-old telemetry becomes unavailable.
  Container liveness does not set trading `ready`; clock sync remains unmeasured.
- Local daily capture uses the existing two-day/50-MiB writer. Test containers were
  stopped, with durable volumes and ignored evidence retained.

The actual monitor CLI also passed against the stopped fixture: all 13 services
remain STOPPED/CRITICAL, rather than showing healthy cached readings; all 13 daily
captures are bounded. The merged VPS logging overlay validated all 15 service/init
settings with syslog forwarding and two 25-MB fallback caches
(`storage/paper_final_packaging.log`). This verifies configuration and local capture,
not actual VPS syslog delivery or overnight rollover.

The initial fixture run correctly rejected its two-symbol/top-50 mismatch. The
accepted run aligns only fixture settings to two symbols; production paper stays
top 50. A separate two-candle public Binance probe succeeded from this local
network (`storage/paper_public_probe.json`); this is connectivity evidence, not an
actual-market overnight paper-trading acceptance.

All dashboard Go tests passed with the production CGO-disabled configuration;
market-data time, historical no-lookahead and default LIVE topology audits passed.
Paper runtime/API/frontend container builds passed. Step59 passed again with the
fingerprint below (`storage/paper_step59.log`), 25/25 matches, zero differences and
50 fills. No full-history or slow browser campaign was run.

Paper execution retains service close(T) monetary sizing and simulated open(T+1)
fills, including when starting later in the day. This proves operational wiring,
not achievable live prices, canonical next-open sizing parity or perpetual
collateral/funding economics. The VPS daily overlay covers all 13 long-running
services plus two initializers; the disk planning reserve is now 1.5 GiB for daily
archives and 24.5 GiB total. Actual VPS installation, rollover/trim, recreation,
overnight growth and capacity acceptance remain pending.

The full WSL Meson build passed with 20 configured targets after retiring the
frozen research runtime. Current executable names and binary paths were preserved;
the temporary comparison executable and its two support libraries were removed.
The compact Step59 gate passed after this cleanup:

```text
window             2020-01-01..2020-04-16
days               107
RealTest matches   25/25
differences        0
canonical fills    50
fingerprint        94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2
```

It covers deterministic reruns, system restart at a day boundary, dashboard
equality/restart, and pacing-speed invariance. Previously recorded live structural/
clock/SQLite/identity/deployment audits and historical no-lookahead tests also passed.

## Documentation/validation cleanup verified

On 2026-10-07 the WSL project build and Step59 gate were rerun successfully with the
same compact fingerprint above. Current LIVE structural/clock/daily/SQLite/identity/
deployment checks, the no-legacy-clock audit, historical topology/no-shared-clock
checks and the historical visibility no-lookahead unit test passed.

The dashboard source checker passed 33 contract markers. All 34 API routes are covered
by the updated guide; retained Markdown links resolve. Syntax checks passed for 86
validation/dashboard/handoff shell scripts and the modified Python checkers. The moved
dashboard source guide retains the ledger gate's required contract markers.

Removed 39 obsolete files (10 documentation files), moved five dashboard guides into
`dashboard/docs/` and renamed the current absence audit to
`validation/no_legacy_clock_audit.sh`. Current explanation text was consolidated
into the retained guides and `docs/README.md` index. The historical topology audit
now checks current read-only/query-only consumer ownership while allowing WAL sidecar
access on the existing writable named volume.

Frozen venue document bytes and generated evidence were preserved. No trading algorithm,
configuration or deployed topology was changed by this documentation cleanup.
Browser/full-history/deployed-runtime campaigns were not rerun.

## Research reporting migration verified

On 2026-10-07 the six frozen-runtime consumer sources were inventoried in
`research/REPLAY.md`. `algotrading_research_html` now uses CURRENT library APIs
for PureRSI and research-only Donchian/XH/XH-ATR experiments. The explicit
`algotrading_research_html_legacy` target initially preserved the original experiments;
all useful experiment consumers now have CURRENT paths. XH robustness, initial
parameter studies, statistics, BTC moving-average and isolated scenarios use CURRENT APIs.

The whole-project WSL Meson build passed with 23 configured targets (the additional
target is the explicit legacy HTML fallback). The bounded reporting gate passed
on `2020-01-01..2020-04-16`:

- 107 account snapshots and 25 closed PureRSI campaigns; baseline trade CSV
  byte-identical to the independently configured public fast executable.
- Eight compact sensitivity combinations: six successes, two invalid entry/exit
  rejections, zero failures. Repeated metric rows and HTML were deterministic.
- Existing sensitivity/grid/metadata CSV columns and HTML equity/drawdown,
  historical trade-return, seeded Monte Carlo and sensitivity sections preserved.
- Realized balance and marked equity reconcile to exported campaign PnL.
  Shorter-window account history matches the longer run's prefix; ten-bar
  warm-up yields no trades. Nonzero commissions are netted once.
- Synthetic fixtures verify open-cutoff exposure, closed-only Monte Carlo,
  observed-asset-bar holding duration (including data gaps), and Donchian close-signal/next-open execution
  with gap prices and entry-filter-independent exits.
- The bounded legacy fallback still generates all four original strategy reports.

Observed baseline economic differences are explicit, not regressions masked by
report compatibility:

| Study | Frozen final equity | CURRENT final equity |
| --- | --- | --- |
| PureRSI | 106521.266499368 | 106615.38896244 |
| Donchian | 97702.5021482057 | 97510.0451955678 |
| XH | 101370.95958945 | 100638.894859702 |
| XH-ATR | 91402.5261060157 | 91347.7321060525 |

Donchian retains its close-based signals but exits at next open rather than the
old same-close price. It has not entered the library's validated strategy catalog.
Reporting now derives missing holding counters from observed bars and includes
open campaigns in exposure; closed-trade metrics retain their realized-only basis.

Reviewable generated studies are local and ignored:
`storage/backtests/sensitivity_results/current_html_migration_20261007_rsi/` and
`storage/backtests/sensitivity_results/current_html_migration_20261007_donchian/`.
They include baseline metric CSVs, HTML, execution settings and CURRENT trades/
account snapshots. Validation logs are `storage/research_html_gate.log` and
`storage/research_html_step59.log`.

Step59 was rerun successfully with the accepted compact fingerprint above,
25/25 RealTest matches, zero differences and 50 fills, including deterministic
reruns, system/dashboard restart and paced-speed invariance. No full-history,
browser, private exchange or deployment campaign was run by this migration.

## XH stop-entry migration

The XH migration adds CURRENT one-bar stop entries and causal deferred exits.
Synthetic lifecycle validation passed: trigger/gap prices and monetary sizing,
expiry with missing assets, cancellation, split-fill idempotency, in-memory
restoration, adapter/wire/store rejection before unsafe routing, actual-fill fees,
open cutoffs and ATR ratchet reconstruction excluding entry-bar high.
`storage/xh_migration_stop_test.log` contains this proof. Step59 passed again with
the exact fingerprint above, including all restart/dashboard/pacing checks;
its log is `storage/xh_migration_step59.log`.

Reviewable CURRENT reports for all four definitions are ignored under
`storage/backtests/sensitivity_results/current_xh_migration_20261007/`.
Their XH/XH-ATR final equities are 100638.894859702 and 91347.7321060525,
with 20 and 12 closed campaigns respectively. The expanded 107-day reporting
gate passed, including four compact XH combinations and eight compact XH-ATR
combinations, all successful; legacy/current CSV columns, campaign uniqueness,
account/PnL reconciliation, shorter-window prefix equality and nonzero fees passed.
RSI fast-export equality and deterministic reporting also remain passing.
The whole-project WSL Meson build passed with all 23 configured targets.
Logs: `storage/xh_migration_gate.log` and `storage/xh_migration_full_build.log`.

Stop entries remain an in-process research capability. Service JSON, other
exchange adapters and durable schema v1 reject them. Disk restart and private
conditional routing are not claimed. The frozen XH sources normally look up a
future open while processing a close and fall back to that close at cutoff;
CURRENT waits for the actual later open and leaves cutoff positions marked.
ATR entry eligibility is evaluated at signal close rather than future fill-bar
ATR. No experiment was added to the validated strategy catalog.

## XH robustness consumer migration verified

`algotrading_research_xhbreakout` now links CURRENT `libalgolib` and shared
research XH definitions; build linkage has no frozen-runtime dependency. The
WSL target build and expanded reporting gate passed. Original full grids retain
2,508 XH combinations and 2,299 XH-ATR combinations, verified by enumeration
without executing those full studies. The 107-day compact study completed
eight successful combinations per strategy with zero invalid/failed runs.

Baseline and nonzero-fee trade/account CSVs are byte-identical to the CURRENT HTML
runner. Repeated exports/HTML are deterministic; shorter windows preserve prior
account history. Original sensitivity CSV columns, metadata columns (including
legacy fee factors), enabled axes and spacings remain. A synthetic zero-ATR
combination and an injected factory failure verify invalid/failed CSV rows,
reasons, empty metric cells and counters separately from successful runs.

Reviewable reports are ignored under
`storage/backtests/sensitivity_results/current_xh_robustness_20261007/`.
Build, reporting and release logs are `storage/xh_robustness_build.log`,
`storage/xh_robustness_gate.log` and `storage/xh_robustness_step59.log`.
Step59 passed again with the accepted fingerprint above: 25/25 RealTest matches,
zero differences, 50 fills and all rerun/restart/dashboard/pacing checks passing.
No new library/live behavior or conditional-routing support was added by this
consumer migration. The frozen runtime was retained for the remaining consumers;
the BTC study migration is recorded below.

## Initial parameter and statistics migration verified

On 2026-10-07 `algotrading_research_initial_params` and
`algotrading_research_stats` moved to CURRENT lib APIs and shared seven original
configurations. Their build linkage contains no frozen-runtime dependency.
The initial study retains original ranges, valid-combination filtering, marginal
medians and HTML filenames. Statistics retain logged metrics and an in-memory cache.
Both default to a cold 107-day daily study; full grids require explicit selection.

The WSL whole-project build passed with 23 configured targets. The expanded
`validation/research_html_report_gate.py` passed:

- Seven baselines and 82 valid compact runs; original 37,702 valid full-grid points
  verified by enumeration without running those full studies.
- Deterministic HTML/trade/account/sensitivity exports and exact shorter-window
  account prefixes for all seven strategies.
- Zero/nonzero-fee account reconciliation and long/short net PnL checks; logged
  statistics match the initial study at their published precision and trade counts.
- Initial PureRSI trade/account exports equal CURRENT HTML; existing HTML/fast,
  Donchian, XH and XH robustness checks remain passing.
- Focused fixtures cover short gap notional, bullish/bearish entry-bar protective
  ordering, later protective gaps, actual-fill cover sizing, split-fill/terminal
  idempotency, timed-exit bracket cancellation, missing short-entry expiry, observed
  holding periods and in-memory account/order/campaign restoration.

Reviewable reports and verification exports are ignored under
`storage/backtests/strategy_reports/current_initial_migration_20261007_verified/`.
Zero-fee baseline results for `2020-01-01..2020-04-16`:

| Strategy | Closed campaigns | Final marked equity |
| --- | ---: | ---: |
| BargainChaser | 6 | 102061.20580248954 |
| ATRBreakout | 66 | 101614.36781725891 |
| MRShort | 28 | 97947.91446917891 |
| PureMom | 3 | 95169.61563099793 |
| PureRSI | 25 | 106615.38896244048 |
| MRRSILong | 37 | 102796.47384186377 |
| XHBreakout | 2 | 97709.38798443158 |

CURRENT money comes from fills and executable-price sizing; no legacy future-open
lookup, signal-close quantity rounding or forced cutoff close-out remains in these
consumers. Source entry predicates and timed/stop rules remain explicit, including
MRShort's signal-close-plus-ATR protective stop and its research OHLC assumption.
These are research experiments, not additions to the validated strategy catalog.
Service JSON, unsupported adapters and durable schema v1 reject conditional fields;
stateful research strategies reject durable-store attachment. The in-memory proof
retains analytics alongside operational state; it does not prove disk/live recovery.
Sparse held-asset marking/target resolution still requires valid prices.

Logs: `storage/initial_migration_final_build.log`,
`storage/initial_migration_gate.log`, `storage/initial_migration_verified_study.log`
and `storage/initial_migration_release_gate.log`. Final Step59 passed with the
accepted fingerprint above, 25/25 RealTest matches, zero differences, 50 fills and
all deterministic/restart/dashboard/pacing checks passing. Full-history, slow browser,
deployment and private-exchange campaigns were not rerun.

## BTC moving-average migration verified

On 2026-10-07 `algotrading_research_btc_ma` moved to CURRENT lib APIs, reusing
the shared short/momentum configurations and portfolio sizing. Its original
baselines match the initial study; only BTC SMA length is swept, from 10 to 200
in steps of 5. There is no separate filter toggle in the source. CSV columns,
parameter medians, HTML filenames and metadata/run counters were retained.
The original sensitivity, parameter-grid and metadata writers were checked
unchanged against the tracked source. Bounded dates, fee/strategy controls,
overwrite protection and current baseline trade/account/metric exports were added.

The WSL whole-project build passed with 23 configured targets. The expanded
reporting gate verified all 78 combinations over `2020-01-01..2020-04-16`,
compact endpoints, deterministic CSV/HTML, baseline/nonzero-fee equality with the
initial study and exact shorter-history account prefixes. Injected invalid and
failed runs retain reasons, empty metric cells and correct counters. Original
full ranges remain in parameter metadata even in compact mode.

The saved bounded study completed 39 successful runs per strategy with no invalid
or failed runs. SMA-50 baselines: MRShort has 28 closed campaigns and final equity
97947.914469178912; PureMom has 3 and 95169.615630997927. Reviewable reports live
under ignored `storage/backtests/sensitivity_results/current_btc_ma_migration_20261007/`.
Logs are `storage/btc_ma_migration_build.log`, `storage/btc_ma_migration_gate.log`,
`storage/btc_ma_migration_study.log` and `storage/btc_ma_migration_step59.log`.
Step59 passed with the accepted fingerprint above and all replay/restart/dashboard/
pacing checks. Full-history, browser, deployment and private-exchange work were not run.

No new library/live behavior was needed. Conditional strategies retain the existing
research-only simulator and recovery limitations. At this stage frozen-runtime linkage
remained only for the explicit HTML comparison and `multi_strategy_main.cpp`; the
runtime was retained until useful consumer migration was complete.

## Isolated scenario migration verified

On 2026-10-07 `algotrading_research_multi_strategy` moved to CURRENT lib APIs for
all eight original strategy families. Each strategy/dataset pair still owns a fresh
strategy, indicator cache, account and campaigns. All 18 dataset identifiers,
filenames, benchmarks and regime-SMA bar counts were checked against the tracked
source, together with all original signal constants and 10% asset sizing.
The scenario-specific Bargain/ATR/short settings remain distinct from the initial study.
Shared short/momentum constructors accept an exact benchmark symbol, retaining BTC
defaults. Chart embedding and correlation presentation moved to the cohesive
`research/src/common/scenario_reports.*`; both original HTML writers are unchanged.

The whole-project WSL build passed with 23 configured targets. The bounded saved
study completed 24 runs with zero failures: eight strategies over 107 daily crypto
bars, 73 stock bars and 107 synthetic 4-hour keys in `20200101..20200416`.
All 18 scenarios remain available; the other real datasets were not replayed.
The focused `validation/research_scenario_gate.py` passed account reconciliation,
long/short net PnL, fees, duplicate-campaign checks, deterministic exports/HTML,
exact shorter-history prefixes and daily baseline/fee equality with the current
HTML runner. A bounded synthetic fixture warms the original 300-bar intraday filter
and exercises both short/momentum strategies with BTCUSDT rather than BTC.
Embedded benchmark curves match the selected input; the report's actual JavaScript
9-by-9 daily-return Pearson matrix, including BTC buy-and-hold, matches independent
calculations through Node without a browser. Missing/empty-input fixtures retain
skipped/failed counters and reasons and return nonzero status.
The existing HTML reporting gate also passed after the benchmark extension:
initial/stats, BTC SMA, PureRSI/fast, Donchian, XH/XH-ATR, robustness and the
focused stop/holding/accounting/recovery fixtures retain their accepted results.

Daily zero-fee results with this consumer's original settings:

| Strategy | Closed campaigns | Final marked equity |
| --- | ---: | ---: |
| BargainChaser | 6 | 101220.53107214371 |
| ATRBreakout | 39 | 102022.55998462629 |
| MRShort | 29 | 120253.6125956341 |
| PureMom | 3 | 95169.61563099793 |
| PureRSI | 25 | 106615.38896244048 |
| MRRSILong | 37 | 102796.47384186377 |
| XHBreakout | 20 | 100638.89485970164 |
| DonchianBreakout | 10 | 97510.04519556779 |

Reviewable reports and current campaign/account exports are ignored under
`storage/backtests/multi_strategy/current_scenario_migration_20261007/`.
Evidence logs: `storage/scenario_migration_build.log`, `scenario_migration_gate.log`,
`scenario_migration_study.log`, `scenario_migration_contracts.log`,
`scenario_migration_errors.log`, `scenario_migration_existing_reports.log` and
`scenario_migration_step59.log` under `storage/`.
Step59 passed with the accepted fingerprint above and all replay/restart/dashboard/
pacing checks. No full-history, browser, deployment or private-exchange work was run.

Synthetic intraday keys retain their original ordering; they are not reinterpreted
as real exchange timestamps. Annualization is explicit per bar frequency (252 for
stocks, 365 for daily crypto, intraday/multiday scaling). This remains isolated
research with existing conditional-order/recovery limitations, not validation of
a combined portfolio or new live strategies. At this point the frozen runtime
remained only for the explicit HTML comparison; its retirement is recorded below.

## Frozen research runtime retirement verified

On 2026-10-07 the temporary HTML comparison target, its conditional implementation
and all 62 frozen-runtime files were removed after all six useful consumers migrated.
Meson introspection confirms every consumer links CURRENT `libalgolib`; the whole
WSL build passed with 20 targets, removing the comparison executable and its two
support libraries. Current executable names and paths remain unchanged.

A bounded four-strategy study before and after removal produced byte-identical
HTML, trade exports and account history. Baseline metrics, parameter grids,
metadata and execution settings also match, excluding study identity/creation time.
Reports remain under ignored `storage/backtests/sensitivity_results/` in
`runtime_retirement_before_20261007/` and `runtime_retirement_after_20261007/`.
An ignored local archive preserves the removed files, including the modified README:
`storage/frozen_runtime_before_retirement_20261007.zip`.

The revised reporting gate passed without building or running a frozen engine.
It checks explicit CSV columns and HTML sections, all four CURRENT selections,
PureRSI/fast parity, accounting/fees/cutoff, deterministic reruns, causal prefixes,
initial/statistics parity, bounded BTC sensitivity and XH robustness/failure contracts.
Existing focused metrics and conditional-order fixtures also passed. No library/live
trading behavior changed. Research-only conditional-order/recovery limits remain.

Step59 passed with the accepted compact fingerprint above and all parity,
rerun/restart/dashboard/pacing checks. Evidence is in `storage/`:
`runtime_retirement_build.log`, `runtime_retirement_reports.log`,
`runtime_retirement_contracts.log`, `runtime_retirement_before.log`,
`runtime_retirement_after.log` and `runtime_retirement_step59.log`.
No full-history, slow browser, deployment or private-exchange work was run.

## Generated repository navigation verified

On 2026-10-07 `tools/generate_ai_index.py` generated three ignored local files:
`.ai/README.md`, `.ai/components.md` and `.ai/index.json`. The current snapshot
covers 628 files across 56 component directories. Its 20 literal Meson target
names match the configured build's introspection output. The Windows and WSL
scans produce identical index data; generated Markdown and include/reverse/candidate
links resolve to existing indexed source. Source bodies and absolute workspace
paths are not copied into the output.

Four focused WSL fixtures passed: force-tracked credentials/ignored paths and
symlinks stay excluded, stale edits/additions/deletions are detected, refreshes
are deterministic, CRLF/LF produce identical hashes, ambiguous includes remain
candidates and unrelated local `.ai/` notes are preserved. The generation guide
is `docs/codex/AI_INDEX.md`; original guides still own architecture, priorities
and accepted validation. No trading source or build definition changed.

The actual CLI detected guide edits with exit status 1 without changing outputs.
After refreshing the final guides, the freshness check passed. Step59 was rerun
under WSL and passed with the accepted compact fingerprint and all parity,
rerun/restart/dashboard/pacing checks. No full-history or browser campaign ran;
the whole Meson build was not repeated because compiled sources/build definitions
were unchanged.

Evidence logs under `storage/`: `ai_index_tests.log`, `ai_index_generation.log`,
`ai_index_stale.log`, `ai_index_freshness.log` and `ai_index_step59.log`.

## Daily service logging implemented and locally verified

The initial five-day policy below was superseded on 2026-10-08 by the user's
two-day, size-bounded policy; see the current validation entry below.

On 2026-10-07 optional LIVE/dashboard Compose overlays and a dedicated loopback
rsyslog receiver were added. The host archive uses UTC receipt-day files under
`/var/log/algotrading/services/`, with private permissions, defensive credential
filtering and append across receiver restarts. An hourly persistent systemd timer
retains today and the previous four UTC days, including quiet services. Cleanup
preserves symlinks, unknown files and trading journals/checkpoints. The overlays
force C++ stdout logging instead of optional unbounded application file output.
Base Compose settings and trading source remain unchanged.

Four focused WSL tests passed using an isolated real rsyslog TCP receiver and
temporary fixtures: retention/safe cleanup scope, restart/append and credential
filtering, UTC date templates, and all eight LIVE/two dashboard merged logging
settings. Permissions were checked. Compose rendering used dummy values; no LIVE
containers were deployed. The date-boundary fixture substitutes reported time
into the production date template; actual UTC receipt-time rollover on the VPS
remains an acceptance check. The LIVE topology audit and systemd unit verification
passed. Step59 passed with the accepted compact fingerprint, 25/25 matched trades,
50 fills, zero differences and all rerun/restart/dashboard/pacing checks.

Evidence under `storage/`: `daily_logs_tests.log`, `daily_logs_topology.log`,
`daily_logs_units.log` and `daily_logs_step59.log`. No full-history or slow browser
campaign ran. Installation instructions and delivery limits are in
`deploy/live/README.md`: the non-blocking forwarding buffer can lose diagnostics
during prolonged receiver outages, and archive filtering does not sanitize
upstream Docker caches. This archive is separate from durable trading evidence.
Actual VPS installation, Docker-to-host delivery, container recreation, real UTC
rollover and scheduled expiry remain pending; nothing was deployed.

## Local VPS equivalence baseline verified

On 2026-10-07 the current WSL whole-project Meson build passed and the LIVE
runtime bundle was regenerated with six executables and 39 shared libraries.
A separate `algotrading-runtime:vps-baseline` image built successfully; all six
packaged binaries exited successfully with `--help` in read-only containers with
no network, capabilities or trading-data mounts. Existing deployment image tags
were not replaced, and no trading services were started.

The LIVE topology audit and four daily-logging fixtures passed. Step59 passed
again with the accepted compact fingerprint, 25/25 matched trades, zero
differences, 50 fills and all rerun/restart/dashboard/pacing checks. Replay input
and default LIVE market/strategy/portfolio configuration checksums, actual Meson
compiler/build options, local tool versions and the validation image ID were
captured under ignored `storage/`. Source navigation is refreshed separately;
the index excludes credentials/generated state and is not a deployment manifest.

Evidence: `vps_baseline_build.log`, `vps_baseline_bundle.log`,
`vps_baseline_topology.log`, `vps_baseline_logging.log`,
`vps_baseline_step59.log`, `vps_baseline_image_build.log` and
`vps_baseline_image_smoke.log`. Configuration/input/toolchain captures use the
same `vps_baseline_` prefix. The build reported existing unused-variable warnings
in the fast research entrypoint; no compiled source changed in this task.

`deploy/live/README.md` now defines the local-to-VPS comparison procedure.
Actual VPS verification remains pending: no SSH host/user/repository location was
provided, no remote connection was made and no deployment was performed. The
local image smoke check does not prove deployed messaging/database recovery or
private exchange connectivity. No full-history or browser test ran.

## Isolated transport/persistence crash recovery verified

On 2026-10-07 `validation/transport_persistence_integration_test.py` passed under
WSL against real, disposable NATS 2.10/PostgreSQL 16 containers, compiling the
current `JetStreamBus` and `PostgresStateStore` source. A forced SQL constraint
failure after fill insertion rolled back both the fill and snapshot. The fixture
then committed a fill/snapshot and abruptly exited before ACK. After broker and
database kill/start, a fresh consumer recovered exact cash, position, decision/
execution timestamps, next-order identity and processed fill IDs, rebound to its
wildcard durable subscription and ACKed redelivery without another economic delta.
Duplicate publication/audit insertion stayed singular; an incompatible durable
filter was rejected. A second infrastructure kill/start preserved the consumed ACK
state and account/fill evidence. All UUID-owned test containers and their anonymous
volumes were removed; the final cleanup capture is empty.

The fixture handler owns economic deduplication using recovered processed-fill
IDs. The store's unique fill audit row alone does not make repeated account deltas
safe. This checks actual adapter transactions/delivery and infrastructure restart;
it does not run the five-service LIVE chain, service checkpoint/outbox crash paths,
container recreation, private routing or a VPS. Those acceptance checks remain
pending. No trading source, base deployment or Meson build definition changed.

Step59 passed again with the accepted compact fingerprint, 25/25 matched trades,
zero differences, 50 fills and all rerun/restart/dashboard/pacing checks. Evidence
under ignored `storage/`: `transport_persistence_integration.log`,
`transport_persistence_cleanup.log` and `transport_persistence_step59.log`.
The fixture compiled its affected adapters directly; the whole Meson build was
not repeated after the prior accepted build. No full-history/browser test or
LIVE/VPS deployment ran. The validation/deployment guides and roadmap now identify
the fixture and its acceptance boundary.

## Controlled local service environment prepared

On 2026-10-07 `validation/local_service_campaign.py` prepared an isolated Docker
project from the current historical topology, with its own internal network,
NATS/PostgreSQL/SQLite volumes, pinned runtime image and generated test credentials.
The seven current service binaries were built under WSL and packaged successfully.
Existing LIVE/historical state, image tags and trading source were preserved.

The bounded synthetic fixture contains 111 BTC/ETH daily rows per coin. Only the
April 19 close creates the economic bootstrap cycle; April 20 supplies execution
opens. The smoke check and raw duplicate injections passed: one checkpoint per
strategy/risk/planner at `20200419`, two fills at `20200420`, no extra fill IDs,
drained outbox and matching execution/backend cash and positions. This proves a
bounded simulated service chain, not private exchange or strategy performance.

The outbox publication-receipt crash fixture passed after recovery. It observed the
SQL barrier after publication, killed the backend, verified pending receipts and
recovered all six records without economic duplication. Execution-state container
recreation also preserved the accepted account/fill/checkpoint evidence. CPU/RAM
sampling and summary generation ran; these brief samples do not establish capacity.
On fresh project `algotrading-local-6f2aa7909ee4`, the strategy publish-before-checkpoint
barrier passed: the checkpoint remained absent after the observed crash, and durable
redelivery produced the accepted aligned pipeline and two fills. Both accepted
projects were paused with their volumes and evidence retained.

The harness includes guarded service/infrastructure kill/start/recreation commands,
fresh-project publish-before-checkpoint SQL barriers, an outbox publication-receipt
crash fixture, daily append-only log capture with five-day cleanup and attributed
CPU/RAM samples plus a summary. Its runbook is
[validation/LOCAL_SERVICE_CAMPAIGN.md](validation/LOCAL_SERVICE_CAMPAIGN.md).
Risk/planner checkpoint barriers, infrastructure loss/recreation, the complete fault matrix,
overnight rollover/scheduled expiry and full-backtest/live resource profiles remain
pending campaign acceptance. A manually restarted service does not prove unattended
recovery. Local log capture does not install the VPS rsyslog/systemd receiver.

Preparation initially rejected a CRLF synthetic CSV header; the fixture now writes
LF explicitly and a fresh project passed. Host TCP publication was blocked by the
internal network; duplicate injection now runs an SDK client inside that network.
Restart checks exposed unordered order-map/fill-set serialization: the checker sorts
these two collections for comparison while preserving every record and duplicate.
The original saved baseline remains intact; cash, positions, fills and checkpoint
payloads must still match exactly.
Checks already running with the old comparison timed out after recreation; a fresh
run with the corrected comparison passed, including a repeated outbox crash.

The whole WSL build passed. A redundant second compile encountered a recovering
Ninja dependency cache on the Windows-mounted build directory and was stopped;
the missing ignored library artifact was restored from the just-validated bundle.
Fresh sandbox packaging reused that accepted build with `--skip-build`. Use a
configured Linux-filesystem `BUILD_DIR` for sustained campaigns. This workaround
does not accept a failed source build or permit reuse after compiled-source changes.

Step59 passed with the accepted compact fingerprint, 25/25 matches, zero differences
and 50 fills, including restart/dashboard/pacing checks. Four daily logging tests
and four generated-index tests passed. Evidence stays ignored under
`storage/local_service_campaign/`; the accepted sandbox is
`algotrading-local-39ab90a639d5`. Build/fixture failures were retained separately,
and only the failed generated project's containers/network/volumes were removed.
Read-only guard checks rejected repository/LIVE paths and a remote Docker endpoint.
No full-history/browser test, VPS deployment or private trade ran.

## One-command local CPU/RAM study implemented and validated

On 2026-10-07 `research/replay.py resources --days 100 --seconds 300 --open`
was added to the current public CLI. It reuses canonical dashboard replay with
original inputs and direct RealTest comparison in a separate container while the
complete synthetic service sandbox plus API/web/watchdog/test-file notifier runs.
The dashboard observes canonical simulation state; it does not reinterpret the
synthetic service fixture as a 100-day economic backtest. Every run owns its
project, loopback web port, volumes, images, copied runner and output/state paths.

The five-minute WSL campaign passed on project `algotrading-local-0dcbdd742311`:

- Measured `300.000277` seconds, with 295 complete snapshots of all 14 containers.
- Original-data replay covered `2020-01-01..2020-04-09` (100 days), completed around
  160.64 seconds, produced 40 canonical fills, and ended CLEAN/route-safe.
- RealTest: 22/22 matches, zero differences; 18 closed campaigns and four open
  campaigns compared under the existing partial-cutoff entry-only contract.
- Authenticated dashboard client: 349 successful reads and five failures during
  initial simulation-state warmup. All four dashboard services were healthy while
  the remaining period was measured. The generated project stopped automatically;
  a subsequent Docker query confirmed no running project containers.
- Container working-set RAM: mean 112.17 MiB, sampled peak 126.77 MiB. CPU sum:
  mean 12.94%, sampled peak 46.14%, where 100% represents one logical core.
- Separate Windows samples were available: whole-host CPU mean 25.4%, sampled
  peak 100%; used host RAM mean 9957.99 MiB and peak 10393.46 MiB. These include
  unrelated applications and observer overhead. Browser/VM process working sets
  overlap host memory and must not be added to container totals.

Setup/build/startup and shutdown/comparison happen outside measurement. Deliberate
display delays add roughly 25% of the selected duration to processing; this is a
bounded observational load, not maximum-speed throughput or a VPS-sizing acceptance.
Existing dashboard CPU/RAM limits remain. The watchdog retains its required public
TESTNET/dry-run settings with venue egress blocked by the internal network; no private
order or Telegram delivery is enabled. Remaining failure/recreation/overnight
retention tests are deferred to the VPS at the user's request.

`research/resource_profile.py` owns lifecycle/sampling/read-only load;
`resource_report.py` writes offline selectable/hoverable graphs and sample summaries.
Missing/stale observations remain gaps, and failures retain partial HTML evidence.
Earlier short attempts rejected incomplete replay/coverage; Docker Desktop terminal
redraw sequences are now removed before parsing. A corrected 20-day/30-second
campaign passed, including Windows sampling and automatic shutdown. Four focused
report tests and a generated-JavaScript syntax check passed. Credential environment
and key files are excluded from both dashboard Docker build contexts, including
nested files. Generated reports, telemetry, credentials and replay exports are ignored.
After copying the runner into each private run and adding end-of-window service
checks, a final 35-day/60-second campaign (`algotrading-local-777efdd1d8f3`) passed
with actual fills and 5/5 RealTest matches, zero differences. It verified the final
code path, report formatting and automatic shutdown without repeating five minutes.
Four generated-index fixture tests also passed.

Step59 passed again with the required compact fingerprint, 25/25 matches, zero
differences and 50 fills, including all restart/dashboard/pacing checks. Its log
is ignored `storage/resource_profile_step59.log`. No full-history run, slow browser
campaign, VPS deployment, private trade, commit or push occurred.

## Earlier evidence, not rerun by the readability cleanup

Full history: `2020-01-01..2025-10-13`, 2113 source days, 631 candidate
trades, 628 fully matched, four comparison differences, 1261 fills.

```text
1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0
```

The differences are the BNB exit date, missing candidate FET trade, final ZEC
end-state/time, and extra candidate FET trade. Their review was closed by the user
on 2026-10-08 based on closing conventions and RealTest RSI rounding. They remain
visible; this acceptance does not establish exact parity or add a mismatch whitelist.
Earlier full-history dashboard/system
fills were byte-identical.

A warmed later-date pacing proof recorded:

```text
b85026fe0cf5206dc4120af9b3a12cdc6a2a978d08faf69b0dc99b602bbe9024
```

These recorded proofs do not imply that every legacy validation script is usable.

## Current code and readiness

- Current library filenames are descriptive; supported source no longer uses
  version suffixes. Wire/schema/version labels remain unchanged.
- PureRSI is the only supported strategy under `lib/src/strategy/validated/`.
  `on_hold/` experiments are excluded from the supported build.
- Live services retain cohesive state machines and meaningful local components.
  There is one source-level build file per service.
- `research/replay.py {fast|system|dashboard}` is the public replay interface.
- `.ai/` provides generated local source navigation; refresh/check it with
  `tools/generate_ai_index.py`. Its outputs remain ignored by Git.
- The frozen research runtime and comparison target have been removed.
  The HTML source builds CURRENT PureRSI and research-only Donchian,
  XH and XH-ATR reporting with explicit contract/accounting regression checks.
  XH robustness, initial parameter, statistics, BTC moving-average and scenario
  sources now link CURRENT lib. All six useful consumers have current paths.
- Dashboard private routing remains blocked. Public venue metadata does not
  establish private account/order connectivity.
- Slow browser acceptance, deployed NATS/PostgreSQL integration, VPS equivalence,
  private TESTNET/shadow and MAINNET readiness remain separate outstanding work.
- Local NATS/PostgreSQL adapter integration and bounded simulated service
  pipeline/duplicate checks pass; complete service fault acceptance remains pending.
- Persistent service/day logging and two-day bounded cleanup are implemented and locally
  tested; installation and acceptance on the VPS remain pending.

## Local disk capacity inventory

On 2026-10-08 WSL `du`, file metadata, `docker system df -v` and selected image
inspection completed without starting/stopping containers, pruning or changing data.
Evidence is ignored `storage/disk_capacity/`: `inventory.json`, `file_sizes.tsv`,
`component_sizes.tsv`, `docker_usage.txt` and `docker_summary.jsonl`. Measurements:

| Item | Observed size / scope |
| --- | --- |
| LIVE runtime bundle | 92,566,236 bytes (88.3 MiB) |
| Selected current-equivalence runtime, profile API/web, PostgreSQL and NATS images | About 1.26 GB summed `system df` rows; conservative because layers are shared |
| LIVE configured local market database including WAL/SHM | 6,438,544 bytes (6.14 MiB); not a proposed VPS state restore |
| Canonical original-data CSV + RealTest reference | 9,772,201 bytes (9.32 MiB) |
| Local research database/data collection | 6,919,188,970 bytes (6.44 GiB), mostly parquet/intraday files not used by default LIVE |
| Five-minute sandbox named state | Market 4.235 MB, PostgreSQL 48.96 MB, NATS 106.9 kB, watchdog 5.045 kB, notifier 10.92 kB |
| Five-minute sandbox captured redacted Compose log | 387,532 bytes; includes setup/shutdown, not a steady log growth measurement |
| All local Docker images / volumes / build cache | 3.96 GB / 693.4 MB / 23 GB; accumulated local experiments, not deployment requirements |

Docker Desktop `image inspect Size` reports different values from `system df` on
this image store; both are retained, and the larger `system df` image rows inform
the deployment allowance. The image IDs are recorded, but local images are not
proof of destination build/toolchain equivalence. Source checks confirmed age-only
five-day log cleanup, nominal roughly 920 MB configured Docker cache/log budgets
with optional gateway and four dashboard services, no explicit JetStream stream
age/byte cap, accumulating durable audit/fill history and download-only symbol
retention rather than OHLCV deletion. No durable retention policy was changed.
The dashboard daily overlay currently covers API/web only; watchdog/notifier still
have size-only production JSON logs. Cover these in the forthcoming VPS configuration
before accepting the all-service persistent daily logging requirement.

The initial 60 GB disk is a reasonable candidate: the updated LIVE runbook allocates
24.25 GiB of planning reserves, leaving roughly 31 GiB before filesystem overhead
on a decimal 60 GB disk. These are estimates, not enforced caps. Actual Linux/package
size, steady-state log/database growth, backup size and VPS capacity remain unmeasured.
Verify them overnight and target at least 15 GiB free. Transfer selected source and
validation inputs rather than the whole local storage/Docker cache; preserve live
state and use a consistent backup if restoring. No VPS was purchased or deployed.
This inventory changed documentation only; no build, Step59 or backtest rerun was
needed. The previously accepted compact fingerprint remains the baseline.

## Two-day bounded service logging locally validated

The initial 10 MiB cap was raised to 50 MiB later on 2026-10-08 at the user's
request. All eight WSL receiver/retention tests passed again, including a real
oversized-file fixture against the 50 MiB default; evidence is
`storage/fifty_mib_logs_tests.log`. The logging threshold and disk-budget figures
changed; the previously accepted Step59 result below was not rerun for this
constant-only adjustment.

On 2026-10-08 the user replaced the original five-day diagnostic policy with
today/yesterday and size trimming. The host receiver now passes redacted UTC
records through rsyslog `omprog` to the existing `clean_daily_logs.py --receive`.
That writer checks the 50 MiB limit before each append, keeps roughly the newest
25 MiB of complete lines when trimming, adds a `LOG-TRIM` marker and continues in
the same dated inode. It never creates numbered/compressed archives. An individual
oversized record is replaced with an omission marker. File locks serialize append
and timer/manual trimming; diagnostic expiry runs on startup, date changes and a
persistent UTC minute timer, including quiet services. Midnight expiry can lag one
timer interval. Today/yesterday are at most two dated files per service, not two
files for the entire stack. Durable trading journals/fills/checkpoints remain outside
this policy. Docker inspection caches retain their separate size limits.

Eight WSL tests passed with rsyslog 8.2112.0 and isolated temporary fixtures:
two-day UTC retention/safe scope, receiver restart/recreation and permissions,
credential filtering, midnight templates, real receiver burst trimming followed
by continued writes/restart, tail/complete-line preservation, oversized-record and
unsafe-path handling, actual default 50 MiB enforcement, and unchanged merged
Compose service settings. LIVE topology audits, Python syntax and systemd unit/
minute calendar checks passed. Systemd reported only an unrelated installed snapd
`RestartMode` warning after checking permissioned temporary unit copies.
Evidence: ignored `storage/two_day_logs_tests.log`, `two_day_logs_topology.log`,
`two_day_logs_units.log`, `two_day_logs_timer.log` and `two_day_logs_step59.log`.
The WSL Step59 release gate passed after this logging change: 25/25 matches,
zero differences, 50 fills and fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`,
including deterministic rerun, system/dashboard restart and pacing invariance.
No full-history or slow browser test ran.

The maintained deployment/dashboard guides and roadmap now use two-day capped
logs. Twelve stable service identities need at most 1,200 MiB of retained archive
contents; the planning archive allowance is 1.25 GiB and total disk reserves are
24.25 GiB. The dashboard daily overlay still needs watchdog/notifier coverage in
the next VPS configuration task. No host units were installed, no VPS was deployed
and no existing evidence or trading state was deleted. Actual Docker-to-host
delivery, real midnight expiry and VPS restart/retention acceptance remain pending.

## Next task

The ordered next steps are in [docs/ROADMAP.md, Next steps](docs/ROADMAP.md#next-steps-2026-10-15-in-this-order):
the authorized release with the equity fix and the VPS row repair, then the cleanup
task A questions, the stale replay bundle, the pending proposals and the first small
Kraken orders. See the [handoff](docs/codex/CONTEXT_FULL.txt),
[architecture](docs/codex/ARCHITECTURE_FULL.md) and [validation guide](validation/README.md)
for navigation and checks.
