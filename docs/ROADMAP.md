# Remaining implementation and acceptance roadmap

## Next steps (2026-10-15, in this order)

**Stop-loss live (2026-10-17, authorized):** release 20261017_190518 with a reduce-only Kraken
stop 30% below each long's last daily close, moved at every close (`LIVE_LONG_STOP_FRACTION=0.30`,
README "Stop-loss"). LIVE state restarted at 23:14 UTC as kraken3 after the positions were closed
manually in Kraken at 15:13 UTC; NEAR/STRK/WLD re-bought with stops accepted.

**Previous VPS release (2026-10-17 14:31 UTC, authorized): 20261017_142931-9bed096-dirty** (Kraken
marks, funding, total equity with BTC share, alert summaries, monitor in LIVE; old releases and
images removed, disk 15%).

**Kraken LIVE running since 2026-10-17 12:59 UTC** with real order submission (namespace
kraken2), test thresholds RSI(8) > 50 / < 40 (`pure_rsi_8_50_40_volume_rank.json`) and the
portfolio file `pure_rsi_equal_weight_rel25.json`; first orders NEAR/STRK/WLD (~20 USD each).
Switch to entry 80 / exit 65 when the user asks (new strategy file + adoption on a running
chain). PAPER data removed from the VPS (archive in storage/vps_paper_archive/).

**LIVE strategy decided by the user (2026-10-17):** PureRSI with RSI length 8, entry RSI > 80,
exit RSI < 65; universe top 20 by SMA(quote volume, 25); candidates ranked by SMA(quote
volume, 25), highest first; max 10 signals; equal weight 10% of equity per signal; a held
position goes back to 10% when |10% - current weight| >= 25% of its current weight (below
8% or above 13.3%); entries and exits always; max 1.5x gross / asset. No volatility target,
no BTC filter. Backtest evidence: CURRENT_STATE.md "PureRSI 8/80/65, 10% equal weight:
relative rebalance thresholds" (Binance perps 2020-2026: 63.0% CAGR, max DD 30.5%, Sharpe
1.53). The chain already supports every part (`equal_weight` sizer, `relative_threshold`
rebalance, `indicator` ranker on SMA QuoteVolume 25): it needs a new strategy file and a new
portfolio file (do not edit the 80/70, 50/40 or vol-target files in place), then
STRATEGY_FILE/PORTFOLIO_FILE and RSI_ENTRY=80 / RSI_EXIT=65 in chain.env at the LIVE switch.

Besides the authorized release with the equity fix and the repair of the two VPS rows
(see the paragraph below and CURRENT_STATE.md):

1. Cleanup task A open questions: the user decides on the "PREGUNTAR" group of
   `storage/cleanup_notes/A-inventory.md` (Parquet files, VPS backups/snapshot, tax
   scripts, the historical runner/audit family). Nothing in it is deleted until then.
2. `research/replay.py fast` fails locally: it prepends
   `deploy/historical_replay/.runtime_bundle/lib`, whose libalgolib (2026-10-11) predates
   `TradingEngine::onBarClose(..., extraCapital)`. Refresh that bundle or stop prepending
   it when the build library exists.
3. Standalone dashboard deployment retirement is user-approved and implemented
   locally: deploy/chain is the deployment entrypoint; shared dashboard behavior
   remains covered by the Go suite and source audit. Remaining proposals awaiting
   the user: remove the empty `Backtester::closeTrades()` stub from lib/; split the
   dashboard-state export out of `research/src/canonical/canonical_replay.cpp` (Step58A
   greps its source text, so adjust that gate in the same change).
4. Kraken: the user deposits more than USD 200; then step (c), the first small real
   orders, each one explicitly authorized (funding sign, average entry price, immediate
   USD settlement of realized PnL, scaled-asset units such as PEPE), and step (d), checking
   fees, funding, net PnL and the tax CSV against the Kraken export, SKIPPED markets and
   the Live vs Expected explanations.
5. Local standby of the deployed version (user request 2026-10-15): every time a release
   is activated on the VPS, keep locally, in its own folder (not live_trading/), what is
   needed to reproduce what the VPS does with the fast backtest (same source/release,
   strategy and portfolio configuration, seed account/positions), so that if the VPS goes
   down the user can compute locally the decisions/targets the VPS would have taken.
   Agreed design (2026-10-15): stateless. Each run downloads the last 100 daily bars of
   every current Binance USDT perpetual, runs the deployed strategy/portfolio configuration
   over them, reads the Kraken account at that moment (positions, equity, BTC collateral)
   and derives the orders as target minus holdings, with the same 20% relative resize rule;
   the only VPS-owned input is the Binance -> Kraken symbol map (already in the repo). It
   may then send the orders to Kraken from the local machine: never while the VPS is alive,
   only after the user confirms the listed orders (real-order rule), and only once Kraken
   LIVE is the deployed mode. Plan-only mode first; validate its targets against the VPS on
   the same day before any order is sent from local.
   Phase 1 done locally 2026-10-15/16 (`standby/`, README there): `sync` copies the deployed
   profile from the VPS read-only; `plan` downloads the data and prints the orders, never
   sends. Its plans for closes 20261013-15 equal the VPS PAPER decisions and order amounts to
   the cent. Remaining: phase 2 (sending from local, with a trading key, the VPS-down check
   and per-run confirmation) once the chain trades Kraken LIVE; refresh `sync` after each
   release.
6. Resize rule (user decision 2026-10-15, done locally, pending release): with the vol-target
   portfolio a held position is resized only when its target weight differs from its current
   weight (at the current price) by at least 20% of that weight (`relative_threshold` 0.20 in
   `config/portfolio/pure_rsi_vol_target.json`; was an absolute 2-point threshold). Entries,
   exits and side changes always happen. On the VPS the portfolio file must be edited and
   adopted with the README "Changing the portfolio sizer" procedure after the release.
7. Telegram for the general watchdog alerts (user decision 2026-10-16, instead of a second
   healthchecks.io check): every watchdog alert goes to Telegram, CRITICAL to the critical
   group and the rest to the normal chat, and a CRITICAL one only after it has lasted 5
   minutes (restarts stay silent). Done locally: `DASHBOARD_NOTIFIER_CRITICAL_DELAY` 5m
   for the general notifier. On the VPS, after the release: `NOTIFIER_SINK=TELEGRAM` in
   chain/chain.env and restart only the notifier (needs authorization).

Latest VPS release (2026-10-16, authorized): 20261016_095921-9bed096-dirty with everything
listed below as local, then the equity-row repair, the 20% rule adopted in the VPS
portfolio file and the general watchdog notifier on TELEGRAM with the 5-minute critical
delay (steps 6 and 7 done; CURRENT_STATE.md has the evidence). One WARN "Risk decision
checkpoint unavailable" stays active until the 00:00 UTC decision after the adoption.

Previous release (2026-10-15, authorized): 20261015_090303-d11699d-dirty with the
Binance `/fapi/v1/time` day-close guard, the Kraken monitor KRAKEN_UNREADABLE warning and
account-history summary, and the notifier "Alert source stopped" warning. Ledger unchanged;
CURRENT_STATE.md has the evidence and a pre-existing equity-row overwrite found on restart.

Local since that release (2026-10-15/16, not deployed): the equity-row fix (first row of a day
wins; step 11), removal of the Hyperliquid TESTNET gateway mode/dashboard path, the
repo cleanup (storage/cleanup_notes/), the 20% resize rule, the heartbeat files, the standby
the comparison-baseline fix for a held coin outside the top 50 and the 5-minute critical
delay of the general notifier (CURRENT_STATE.md). Next release must carry the fix; afterwards repair
the VPS rows 20261013/20261014 with the procedure in CURRENT_STATE.md (needs authorization).

Full deployment update (2026-10-14): explicitly authorized and completed as
20261014_201725-d11699d-dirty. Runtime root is now `/algoTrading`; all eight folders,
active comparison paths, host units/logging/AppArmor and current images are updated.
Fourteen containers healthy, authenticated dashboard/private HTTPS pass, economic
ledger unchanged after restarts. Five retained comparison rows have zero signal
differences. Critical-only Telegram routing and colored/silent policy are active
for Kraken; the general watchdog sink remains TEST_FILE. Actual running former
images are preserved as rollback target pre-root-20261014_201026 (not exercised).
Daily cycles, scheduled summary/reconciliation, faults and private trading still
need their own acceptance. CURRENT_STATE.md owns exact validation evidence.

Updated: 2026-10-14. This guide owns the ordered remaining work and user requirements.
Latest user authorization advances the forward PAPER/shadow study and fast baseline
comparison to the VPS, with a separate PureRSI(7) entry > 50 / exit < 40 profile and
dashboard comparison. Earlier blanket VPS deferral does not block this requested
study deployment. Preserve original profiles/state and keep private orders disabled.
The study is deployed and its first day/API/browser/collector-restart checks pass.
Next observe several UTC daily cycles and investigate retained signal/execution
differences; multi-day acceptance and private integration remain pending.
[CURRENT_STATE.md](../CURRENT_STATE.md) owns validation evidence; component guides
and actual source describe implemented behavior. Entries below are pending work,
not claims that a capability is implemented, broken or ready for live capital.

Research runtime migration and generated `.ai/` navigation are complete. Preserve
the accepted Step59 fingerprint and CURRENT economics during future changes.

PAPER liquidity now uses actual completed daily quote turnover in USDT, averaged
over 25 rows, rather than base-unit volume. Its separate profile preserves the
historical reference. Observe the configured future-candle cutover and accumulate
enough new-metric observations before accepting rolling comparisons.

The controlled local campaign and bounded resource report are already prepared;
see [local acceptance](../validation/LOCAL_SERVICE_CAMPAIGN.md). Local success does
not complete VPS-specific or private-venue acceptance. The VPS study deployment
was explicitly authorized and is running. This roadmap update authorizes no further
remote deployment, restart, configuration change or private orders. Implement future
changes locally, preserve the running study, and obtain authorization for rollout.

## Ordered remaining steps

Baseline prerequisite (2026-10-14): the first volatility-target cycle's four signal
differences were reproduced as an empty strategy seed copied from execution state.
Date-bound strategy seeding, historical repair with preserved evidence and cache
invalidation are deployed (CURRENT_STATE.md). Retained study recalculation shows
zero signal differences; superseded deployment source is retired. Continue daily
cycle acceptance without treating zero signal differences as exact PnL parity.

Completed preparation is recorded in CURRENT_STATE.md rather than listed as pending:
research migration, generated navigation, local charts/timeframes, Pipeline/Risk,
external process/NTP observations, bounded browser/backup/connectivity checks,
Telegram delivery and warning-only Kraken account monitoring, coverage analysis,
and the initial VPS PAPER activity study/fast comparison. Multi-day and private
acceptance remain separate. Mobile access was requested again on 2026-10-14;
private Tailscale HTTPS access and the mobile layout are deployed and accepted
by the user (see CURRENT_STATE.md and deploy/chain/README.md). The user closed the four
historical differences as final-position closing conventions and RealTest RSI
rounding. Preserve raw differences without claiming exact full-history parity.

Account policy remains warning-only: net venue margin equity below USD 50,
available USD below 10% of equity, daily equity decline of at least 10% versus the
preceding UTC day's last observed equity, gross confirmed exposure above 5x, and (user
request 2026-10-14) a fall of less than 25% of BTC and the net long positions that would
reach maintenance margin.
Do not add automatic stops/closures or new opening-order limits. Preserve the
volatility-target sizing the user chose on 2026-10-13 (`pure_rsi_vol_target.json`)
and the 1.5x gross strategy constraint. Daily Telegram
holdings already include their USD notional as a percentage of snapshot equity.

1. Observe several UTC daily cycles of the deployed 50/40 PAPER study; verify signals, positions, cash/equity deltas, collector freshness and restart deduplication against the aligned CURRENT fast baseline. Keep close-versus-open sizing differences visible and preserve original 80/70 profiles/state.
2. Gather and implement the user's forthcoming dashboard adjustments once specified; maintain this as a separate pending task without inventing UI requirements. Done locally on 2026-10-11/12: host CPU/RAM/disk history charts, Grafana-style equity charts, and every page reviewed, Reconciliation last (CURRENT_STATE.md); they reach the VPS with the next authorized deployment. Remaining local items: the user reviews and commits the uncommitted work.
3. Readable code logs (user decision 2026-10-10): every lib/, live_trading/ and research/ log line uses `YYYY-MM-DD HH:MM:SS.mmm [LEVEL] Class::function : message` in UTC, with long IDs/hashes/unrounded values only in DEBUG. Implemented and WSL-accepted locally (build, Step59 fingerprint, samples, tests; see CURRENT_STATE.md). Dashboards are out of scope. The local host receiver source now stores lines unchanged; installing it on the VPS needs a new explicit authorization and the actual UTC-rollover/trimming acceptance stays in step 17. The today/yesterday, 50 MiB per file retention policy is unchanged.
4. Runtime root and readable paths: implemented and validated locally on 2026-10-11 in `deploy/chain/` (see its README and CURRENT_STATE.md). Root `/` on the VPS with /chain, /config, /secrets, /data, /logs, /reports, /backups, /releases; short service names; logs `/logs/<service>/YYYYMMDD.log`; one compose.yml and the `algo` launcher. Migration prepared and rehearsed locally on 2026-10-11 with a real VPS snapshot (`chain/migrate_paper.py`, `algo release`, chain systemd units, comparison collector moved to `deploy/chain/comparison.py`; runbook in deploy/chain/README.md). Run on the VPS on 2026-10-13 with the PAPER history kept (CURRENT_STATE.md). Kraken account monitor/Telegram added to the chain on 2026-10-13 (see step 14); old VPS stack removed the same day with a full archive in /backups (CURRENT_STATE.md). Superseded deployment files retired locally on 2026-10-14 at the user's request: isolated test Compose moved to validation/fixtures/paper, runtime bundle moved to deploy/chain, old launcher/units/overlays/LAN bridge removed. Remaining: accept the chain daily cycles; this source cleanup does not accept multi-day PAPER or authorize deployment.
5. Kraken private test environment: no demo; private integration is tested with small real-money orders, each explicitly authorized; nothing may silently switch from a test endpoint to live submission.
6. Kraken private integration: implemented offline/read-only on 2026-10-11 (`live_trading/exchange_gateway/src/venue/`, guide live_trading/exchange_gateway/README.md): common LiveVenueAdapter + KrakenFuturesProtocol, durable journal, submission gate, idempotency/ambiguity/restart handling, fills, cancels, snapshots; offline tests and a real GET-only probe pass. Remaining: a trading key and the first explicitly authorized small real-money tests (submit, partial fill, cancel, restart, reconciliation), confirming the open points listed in that guide; decide fee capture (roadmap 9). Chain venue mode done locally 2026-10-14: `MODE=live` in chain.env runs `gateway-kraken` (venue mode, `/secrets/kraken.env`, journal in `data/live/venue`) instead of `gateway` + `sim-exchange`, with all trading state in `data/live/`, submission disabled by default and `algo` refusing a mixed start (deploy/chain/README.md "Switching to live").
7. Skip intended perpetuals unavailable on the selected exchange: implemented in the adapter (MARKET_UNAVAILABLE, MARKET_NOT_TRADING, BELOW_MINIMUM_QUANTITY; no substitution; transport failures stay retries). Dashboard done locally 2026-10-14: Execution shows them as SKIPPED orders with the reason and what to do, apart from real rejects, plus INFO alerts (CURRENT_STATE.md). Remaining: verify them in the first live cycles.
7b. Contract units between source and venue (user request 2026-10-12): a Binance perpetual such as `1000PEPEUSDT` is quoted and sized per 1000 PEPE, while Kraken trades `PF_PEPEUSD` per 1 PEPE; a wrong conversion makes the venue order 1000 times too small or too large. Done 2026-10-13 (see `live_trading/exchange_gateway/README.md`, "Symbol map"): (a) the table `config/venues/kraken_futures/symbol_map.json` is built by `tools/kraken_symbol_map.py` from public instrument names plus a same-moment Binance/Kraken price-ratio check (5%), unknown prefixes and mismatches are not mapped; (b) it is the single source for the gateway (`venue/symbol_map.*`, the hard-coded `SCALED_ASSETS` list is gone), `kraken_shadow.py` and the dashboard; (c) gateway tests convert quantities, prices, fills and snapshot positions both ways, including a wrong-scale table; (d) the gateway rejects an order whose venue USD value differs more than 1.5x from source quantity x `ExecutionOrder.reference_price` (`NOTIONAL_MISMATCH`, ALERT); (e) the dashboard Symbol Mapping page shows source symbol, Kraken contract, conversion, both prices and their ratio, and raises `SYMBOL_MAP_*` alerts (wrong scale CRITICAL; strategy asset not on Kraken WARN, CRITICAL while its signal is active). Remaining: (c') re-check with the first authorized real fill on a scaled asset; (e') show venue units in Positions and reports once Kraken trades exist; (f) a table per extra venue (Hyperliquid `kPEPE`-style names) if one is added; (g) refresh the table with the tool when Binance or Kraken list new contracts (the dashboard shows which strategy assets are missing).
8. Complete funded Kraken Multi-M BTC/USD collateral economics: valuation/haircuts, margin, USD settlement, conversion/interest and liquidation-risk reporting. Preserve the agreed warning-only account policy; do not automatically sell BTC or rebalance reserves. User decisions 2026-10-14: the strategy sizes with USD + BTC after Kraken's haircut; USD deposits, withdrawals and conversions are capital flows (not PnL), venue interest a cost. Done locally 2026-10-14 (CURRENT_STATE.md, live_trading/exchange_gateway/README.md): the gateway snapshot carries Kraken's post-haircut collateral, execution-state fixes it per day and adds it to the daily AccountSnapshot, portfolio risk and Manual Control size with it; the account history books INTEREST and CAPITAL_FLOW entries (BTC movements are collateral-only); `trading_equity_daily` records interest, net flows and collateral; the account monitor reports BTC collateral, maintenance headroom and the drop to maintenance margin (daily Telegram, `account.json`, LIVE Overview cards). The user then asked for a warning below a 25% drop to maintenance (MAINTENANCE_DROP, warning-only). Confirmed 2026-10-15 with the read-only key (CURRENT_STATE.md, live_trading/exchange_gateway/README.md): funded `collateral` = BTC `value` x (1 - 1% haircut), margin equity = BTC collateral + USD; wallet fields `quantity`/`available`/`value`/`collateral`; history words "futures trade", "conversion", "cross-exchange transfer"; `old_balance`/`new_balance` = balance of the row's asset (USD, XBT or the contract's signed position size); with no USD Kraken converts BTC itself to pay fees. Remaining: the interest and plain USD deposit/withdrawal words (none yet in the history).
9. Account for actual trading commissions and perpetual funding payments/receipts in replay/live, with explicit timestamps, currency, sign and attribution. Keep estimates distinct from confirmed costs; missing funding is unavailable, not zero, and costs must not be counted twice. Dashboard side done 2026-10-12 (fees/funding per position from the MOCK ledger in the simulation and from fill commissions in PAPER). User decision 2026-10-14: no funding or fee modelling in the PAPER simulation; with Kraken, update the real fees/funding of open positions periodically while trading and store the final values when each position closes. The runtime ledger already attributes `trading_funding` rows to the open position and keeps them in the closed-position row. Done locally 2026-10-14: the Kraken gateway reads the account history (every 5 min, 30 s after a fill), journals each fee/funding entry once and publishes `VenueCostEvent`; execution-state applies it once to cash (`trading_venue_costs`), fees join their fill and funding its position in the ledger; venue snapshot cash is reported spot-style and LIVE reconciliation accepts 0.50 USD (CURRENT_STATE.md). Pending confirmation with Kraken (not documented by Kraken; marked UNCONFIRMED in the code and in live_trading/exchange_gateway/README.md), with the read-only key (`--live-read-only` also reads the account history: path, signing, field names) and the first authorized order: (a) the sign of `realized_funding` (we take it as the change of the USD balance: negative = paid); (b) the fee currency (we book only USD fees; a fee in another currency is logged, not booked); (c) that `openPositions[].price` is the average entry price (used for the spot-style snapshot cash); (d) that realized PnL is settled into the USD balance at once when a position is reduced or closed. If any of these differs, fix the parser/snapshot cash before the next real order. Confirmed 2026-10-15 from the real account history (the user's small trades of 2026-10-07): path/signing/field names; (a) `realized_funding` is the USD balance change, positive = received; (b) fees are paid in USD (positive `fee` on the trade's USD row); (d) realized PnL settles into the USD balance at once (new = old + realized_pnl - fee + realized_funding), and opening moves it only by the fee. Still open: (c) `openPositions[].price` = average entry, `execution` = `/fills` `fill_id`, periodic funding rows while a position stays open. Interest and USD capital flows (step 8) use the same history read.
10. Add separate Commission and Funding columns to the dashboard Trades view, showing USD costs/receipts and net PnL after both. Done locally 2026-10-12 in Positions (active and closed tables: Fees, Funding, Net PnL) plus the closed-positions CSV in `<root>/reports/` for the yearly tax report. Remaining: verify against a real exchange export after the first live trades.
11. Complete runtime-owned cost basis, realized/unrealized PnL and durable balance/equity history, including collateral and funding effects; verify trade totals reconcile to account-level performance. Done locally 2026-10-14: ExecutionState's `PositionLedger` writes `trading_open_positions`, `trading_closed_positions` and `trading_equity_daily` (rebuilt from the fills at every start, missing days backfilled); the dashboard reads them. Fixed locally 2026-10-15: a day's equity row is written once (`insertEquityDay`, ON CONFLICT DO NOTHING) so the restart redelivery of close(T) no longer overwrites it with T+1's fills; pending release, then repair of the two overwritten VPS rows (20261013, 20261014). Remaining: collateral effects (roadmap 8) and checking against a real Kraken export.
12. Verify Manual Control preview, authorization and audit; after private integration is ready, test submission through normal risk/execution, reconciliation and recovery with explicitly authorized small real-money orders. Done in the replay simulation 2026-10-12 (immediate execution, limits from risk config, re-sync at the next close, progress bar, audit). PAPER/LIVE chain done locally 2026-10-14: request folder dash-api -> execution-state, immediate execution at the current Binance prices (PAPER fills at once; LIVE through gateway-kraken only with submission enabled), re-sync to the strategy targets at the next 00:00 UTC cycle (CURRENT_STATE.md). Deployed; remaining: check on the VPS PAPER (one request, then the next night's re-sync), and the authorized real-money test.
13. Extend Live vs. Expected to actual venue execution, costs/funding, skipped markets and collateral constraints using aligned simplest CURRENT PureRSI baseline inputs/settings/dates. Preserve the original raw-volume reference and explain differences instead of forcing parity. Done locally 2026-10-14 (deploy/chain/README.md "Switching to live", CURRENT_STATE.md): `comparison.py` with `MODE=live` reads the Kraken account, real fills and venue costs, takes the Binance open of each execution day, gives the fast baseline the day's BTC collateral and USD capital flows, and each row splits the difference into fees (real vs `LIVE_COMMISSION_RATE`), funding, interest, fill prices vs the Binance open and skipped markets; the dashboard page shows the split. PAPER is unchanged. Remaining: the first real cycles (needs the authorized live test); Manual Control trades are not modelled in the expected run.
14. Persistent Kraken read-only monitoring and Telegram: activated on the VPS with explicit user authorization on 2026-10-14, at the existing root `/`. General READ_ONLY / transfer NO_ACCESS validated; host monitor polls every 30 s with 3-day raw retention, `kraken-notifier` sends warnings and the 09:00 UTC daily summary. Setup message, subsequent fresh account reads and three Telegram delivery receipts pass; existing PAPER container IDs remain unchanged (CURRENT_STATE.md). Remaining: verify the first scheduled daily summary and restart deduplication on the VPS.
15. Formalize versioned Docker-image archive releases and rollback: image IDs/checksums, required Compose/configuration/host units, durable schema compatibility and previous-image restoration. Image export/load already worked for the study; routine VPS builds/repository transfers should not be required. Done locally 2026-10-14 (deploy/chain/README.md "Releases"): `algo release` writes versioned image tags, `release.json` (image IDs, architecture, state format) and its own launcher; `algo activate` verifies, backs up, installs and moves `:chain`; `algo rollback` returns to the previous release; older releases without a manifest are accepted; host units that differ are reported, not replaced. Activated and verified on the VPS on 2026-10-14 as 20261014_201725-d11699d-dirty; actual former running images archived as pre-root-20261014_201026. Remaining: an authorized rollback rehearsal on the VPS.
16. Deploy the remaining accepted local changes to the VPS only when authorized; verify source/input/configuration/image identity and the bounded release gate using the local-to-VPS equivalence procedure, preserving existing state and separate PAPER/private accounts.
17. Finish VPS log acceptance: actual UTC rollover, today/yesterday retention, 50 MiB trimming, recreation and exclusion of secrets. Installation, append/receiver restart, cleanup and boot recovery already passed; durable trading/message/audit growth requires separate planning.
18. Periodic reconciliation done locally 2026-10-14 (`--periodic-reconciliation`, on in the chain): ExecutionState re-checks the exchange hourly 01:00-23:00 UTC and at 23:50, besides startup; size-relative quantity tolerance. Deployed with CLEAN startup reconciliation and durable daily close-loop history; remaining: see the first scheduled checks on the VPS. Complete VPS Infrastructure/dashboard and operational recovery acceptance: runtime-owned responsiveness/heartbeat, synchronized host clock, stale data, NATS/database failures, duplicate handling, reconciliation, backup/restore, unattended reconnect and off-host retention. Local bounded checks do not complete destination-host acceptance.
19. Measure complete-backtest CPU/RAM locally and on the VPS, then shadow/live resource use with duration, peak/typical usage and container attribution. Verify disk growth and keep an initial 15 GiB free-space target; do not transfer the whole research collection/build cache.
20. Accept selected-venue forward shadow across several daily cycles and faults, retaining actual market/rule/price observations and the no-submit boundary. Public/PAPER observations do not accept private fills, margin or reconciliation; complete security, monitoring and recovery readiness before capital.
21. Refresh the Binance top-50/25-completed-day quote-turnover coverage analysis before final exchange integration/selection. Previous snapshot: Kraken 38/50, Hyperliquid main 36/50, with HIP-3 37/50. Kraken Multi-M with mixed BTC/USD is the user's current choice; keep public coverage separate from account eligibility. Refreshed 2026-10-14 (CURRENT_STATE.md): Kraken 39/50 (96.2% of turnover) and all of today's top 20, Hyperliquid 38/50 (39 with HIP-3); symbol map rebuilt unchanged. New `tools/universe_coverage_history.py`: over the last year the strategy's daily top-20 held on average 3.8 assets Kraken cannot trade, with a 7-day RSI above 50 on 266/375 days (1.3 per day) and above 80 on 79 days. Those orders are skipped (step 7); Live vs Expected shows their effect. Re-run before the first live test and when choosing the universe/venue.
22. Clarify the incomplete optional exchange-selection note (`and if its s...`) if the user resumes it; do not invent a criterion or block unrelated work.
23. Run a first explicitly authorized real-money test with a small amount on the selected venue after prerequisites pass; monitor orders/fills, funding, margin, alerts, resources and reconciliation, and confirm the four Kraken points pending in step 9 (funding sign, fee currency, position `price` = average entry, realized PnL settled into the USD balance). No real-money submission is authorized by this roadmap.
24. Increase capital gradually only after the small live test meets agreed acceptance criteria.
25. Continue optional strategy research: OOS results, rankings, correlations, portfolio risk/capacity, fees/slippage/funding, gaps and intraday variants; experiments remain separate from validated strategies.

## Shadow-trading preparation and acceptance

Local preparation is implemented in `live_trading/shadow_observer.py` and
`validation/shadow_trading_test.py`. Run under WSL with local Docker/current images:

```bash
python3 validation/shadow_trading_test.py
```

The bounded PAPER fixture produces actual plans; a loopback GET-only venue spy and
separate SQLite journal check skips, unavailable-data recovery, immutable identity
and duplicate/restart behavior. Reports distinguish producer input from synthetic
negative scenarios and keep unmeasured costs/fills/PnL unavailable. Evidence and
limitations belong in CURRENT_STATE.md. This completes fixture preparation only.
Use the following boundaries for the remaining venue integration/acceptance:

1. Record the strategy, quote-volume universe, completed-candle cutoff, configuration
   identity, virtual capital and reference baseline. Reuse the existing service chain
   and isolated PAPER state; do not create a second strategy/execution implementation.
2. Enforce a no-submit boundary: record proposed orders but never send private
   submit/cancel commands. Start with public/read-only venue data and no trading
   credentials. The local fixture spy verified zero private writes; retain this
   boundary when adding an actual venue reader.
3. Observe venue market availability, symbol mapping, order rules and price freshness.
   Unsupported perpetuals must produce an explicit skip, without spot/venue fallback.
   A transport error is unavailable evidence, not proof that a market is unsupported.
4. Record daily signals, risk decisions, proposed orders and timestamped price/rule
   observations. Compare aligned decisions with the simplest PureRSI backtest;
   modelled fills/PnL remain labelled estimates, never venue confirmations. Fees,
   funding and BTC-collateral assumptions must be explicit; missing inputs are not zero.
5. Prepare bounded local cases for normal/no-order cycles, stale/missing data,
   unsupported markets, disconnect/recovery, duplicate input and restart. Verify no
   real order writes, no duplicate simulated economic events and retained evidence.
6. After coverage/collateral analysis and venue selection, run forward observation
   across agreed daily cycles using actual selected-venue reads. Kraken is preferred
   subject to those checks. Public observation does not accept private order/fill/
   margin/reconciliation behavior; authorized small real-money lifecycle tests remain separate.

Existing PAPER is Binance public data with a simulated exchange, not selected-venue
shadow acceptance. The former Hyperliquid dry-run gateway mode was removed on
2026-10-15; Kraken shadow observation is `live_trading/kraken_shadow.py`. Keep the VPS
and all real-capital execution unchanged.

## Deferred deployment improvement

After the current PAPER observation period, before the next planned release,
prepare deployment from locally built and validated Docker images for the VPS
architecture. Export versioned images with `docker save` to a `.tar`, transfer
them with Compose, required configuration and host logging/monitoring units, then
import with `docker load`. Record image IDs and archive checksums, remove runtime
build requirements from the deployment Compose configuration, and verify the loaded
images before updating containers. Routine releases should not require transferring
the repository or compiling on the 2-GiB VPS.

Keep credentials generated/stored separately on the VPS and preserve its durable
volumes. Retain the previous image set and deployment configuration for rollback;
check state/schema compatibility before reverting. Keep image archives, release
artifacts and credentials excluded from Git. The activity study already used image export/load successfully. The release,
activation and rollback commands now exist (roadmap step 15); their VPS acceptance comes
with the next authorized rollout. Preserve the running PAPER deployment until then.

## Exchange analysis and baseline details

For the coverage study, record the analysis date, exact 25-day window, Binance
market segment, comparable quote-volume normalization, base-asset/pair
deduplication, symbol mappings and data gaps. Use the identical ranked 50-coin
universe for both venue comparisons. Check currently tradable perpetual contracts,
not merely spot listings or a historical catalog entry; keep public market coverage
separate from the user's account/jurisdiction eligibility and BTC-margin support.
Verify venue API, contract and collateral rules from current authoritative sources
when performing that task. The public coverage snapshot and collateral-rule review are
recorded in CURRENT_STATE.md; neither establishes private account/product eligibility.

The incomplete original exchange note remains open, while the coverage comparison
and explicit BTC-collateral requirement can proceed independently. Do not block
unrelated dashboard/logging work on that missing criterion.

The existing lightweight baseline is `python3 research/replay.py fast` using CURRENT
`Backtester` and validated PureRSI. Reuse this baseline rather than inventing another
engine or requiring the distributed/canonical service stack for the simplest
comparison. The existing Live vs. Expected projection covers market/strategy-input anomalies.
The separate PAPER activity comparison already aligns observed daily inputs with
the CURRENT fast engine; multi-day acceptance and actual venue execution/accounting
comparisons still need work.
Do not claim identical results where live fills, funding, fees, skipped markets or
collateral constraints differ. Preserve the independent baseline and explain deltas.

Dashboard symptoms in the user's notes are investigation targets. Source/API
availability, provider mode and UI behavior must establish their causes. Telegram
has an optional adapter; its presence does not prove deployment or real delivery.
Manual submission remains disabled until the required integration and authorization
are in place. See the [dashboard source map](../dashboard/docs/REAL_DATA_SOURCE_MAP.md)
and [production checklist](../dashboard/docs/PRODUCTION_CHECKLIST.md).

Full-history backtests, browser campaigns and live resource profiling belong to
their specific acceptance tasks; do not run them for routine documentation changes.
