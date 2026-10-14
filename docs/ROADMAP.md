# Remaining implementation and acceptance roadmap

Updated: 2026-10-12. This guide owns the ordered remaining work and user requirements.
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
invalidation are corrected locally (CURRENT_STATE.md). The user explicitly deferred
deployment: install and recalculate the retained study only with later authorization,
then continue daily-cycle acceptance and superseded PAPER deployment removal.

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
8. Complete funded Kraken Multi-M BTC/USD collateral economics: valuation/haircuts, margin, USD settlement, conversion/interest and liquidation-risk reporting. Preserve the agreed warning-only account policy; do not automatically sell BTC or rebalance reserves. User decisions 2026-10-14: the strategy sizes with USD + BTC after Kraken's haircut; USD deposits, withdrawals and conversions are capital flows (not PnL), venue interest a cost. Done locally 2026-10-14 (CURRENT_STATE.md, live_trading/exchange_gateway/README.md): the gateway snapshot carries Kraken's post-haircut collateral, execution-state fixes it per day and adds it to the daily AccountSnapshot, portfolio risk and Manual Control size with it; the account history books INTEREST and CAPITAL_FLOW entries (BTC movements are collateral-only); `trading_equity_daily` records interest, net flows and collateral; the account monitor reports BTC collateral, maintenance headroom and the drop to maintenance margin (daily Telegram, `account.json`, LIVE Overview cards). The user then asked for a warning below a 25% drop to maintenance (MAINTENANCE_DROP, warning-only). Remaining: confirm the history `info` words, balance fields and the funded `collateral` value with the read-only key.
9. Account for actual trading commissions and perpetual funding payments/receipts in replay/live, with explicit timestamps, currency, sign and attribution. Keep estimates distinct from confirmed costs; missing funding is unavailable, not zero, and costs must not be counted twice. Dashboard side done 2026-10-12 (fees/funding per position from the MOCK ledger in the simulation and from fill commissions in PAPER). User decision 2026-10-14: no funding or fee modelling in the PAPER simulation; with Kraken, update the real fees/funding of open positions periodically while trading and store the final values when each position closes. The runtime ledger already attributes `trading_funding` rows to the open position and keeps them in the closed-position row. Done locally 2026-10-14: the Kraken gateway reads the account history (every 5 min, 30 s after a fill), journals each fee/funding entry once and publishes `VenueCostEvent`; execution-state applies it once to cash (`trading_venue_costs`), fees join their fill and funding its position in the ledger; venue snapshot cash is reported spot-style and LIVE reconciliation accepts 0.50 USD (CURRENT_STATE.md). Pending confirmation with Kraken (not documented by Kraken; marked UNCONFIRMED in the code and in live_trading/exchange_gateway/README.md), with the read-only key (`--live-read-only` also reads the account history: path, signing, field names) and the first authorized order: (a) the sign of `realized_funding` (we take it as the change of the USD balance: negative = paid); (b) the fee currency (we book only USD fees; a fee in another currency is logged, not booked); (c) that `openPositions[].price` is the average entry price (used for the spot-style snapshot cash); (d) that realized PnL is settled into the USD balance at once when a position is reduced or closed. If any of these differs, fix the parser/snapshot cash before the next real order. Interest and USD capital flows (step 8) use the same history read.
10. Add separate Commission and Funding columns to the dashboard Trades view, showing USD costs/receipts and net PnL after both. Done locally 2026-10-12 in Positions (active and closed tables: Fees, Funding, Net PnL) plus the closed-positions CSV in `<root>/reports/` for the yearly tax report. Remaining: verify against a real exchange export after the first live trades.
11. Complete runtime-owned cost basis, realized/unrealized PnL and durable balance/equity history, including collateral and funding effects; verify trade totals reconcile to account-level performance. Done locally 2026-10-14: ExecutionState's `PositionLedger` writes `trading_open_positions`, `trading_closed_positions` and `trading_equity_daily` (rebuilt from the fills at every start, missing days backfilled); the dashboard reads them. Remaining: collateral effects (roadmap 8) and checking against a real Kraken export.
12. Verify Manual Control preview, authorization and audit; after private integration is ready, test submission through normal risk/execution, reconciliation and recovery with explicitly authorized small real-money orders. Done in the replay simulation 2026-10-12 (immediate execution, limits from risk config, re-sync at the next close, progress bar, audit). PAPER/LIVE chain done locally 2026-10-14: request folder dash-api -> execution-state, immediate execution at the current Binance prices (PAPER fills at once; LIVE through gateway-kraken only with submission enabled), re-sync to the strategy targets at the next 00:00 UTC cycle (CURRENT_STATE.md). Remaining: deploy, check on the VPS PAPER (one request, then the next night's re-sync), and the authorized real-money test.
13. Extend Live vs. Expected to actual venue execution, costs/funding, skipped markets and collateral constraints using aligned simplest CURRENT PureRSI baseline inputs/settings/dates. Preserve the original raw-volume reference and explain differences instead of forcing parity. Done locally 2026-10-14 (deploy/chain/README.md "Switching to live", CURRENT_STATE.md): `comparison.py` with `MODE=live` reads the Kraken account, real fills and venue costs, takes the Binance open of each execution day, gives the fast baseline the day's BTC collateral and USD capital flows, and each row splits the difference into fees (real vs `LIVE_COMMISSION_RATE`), funding, interest, fill prices vs the Binance open and skipped markets; the dashboard page shows the split. PAPER is unchanged. Remaining: the first real cycles (needs the authorized live test); Manual Control trades are not modelled in the expected run.
14. Install persistent Kraken read-only monitoring and Telegram delivery services with startup/restart, deduplication, daily summaries and bounded observation/receipt retention. Done locally 2026-10-13: host unit `algotrading-kraken.service` (30 s polls, raw samples kept 3 days) plus the `kraken-notifier` container (compose profile `kraken`, Telegram, INFO for the daily portfolio); runbook in deploy/chain/README.md. Remaining: create the read-only key file on the VPS and enable it there (needs authorization), then confirm the first warning/daily message and restart deduplication on the VPS.
15. Formalize versioned Docker-image archive releases and rollback: image IDs/checksums, required Compose/configuration/host units, durable schema compatibility and previous-image restoration. Image export/load already worked for the study; routine VPS builds/repository transfers should not be required. Done locally 2026-10-14 (deploy/chain/README.md "Releases"): `algo release` writes versioned image tags, `release.json` (image IDs, architecture, state format) and its own launcher; `algo activate` verifies, backs up, installs and moves `:chain`; `algo rollback` returns to the previous release; older releases without a manifest are accepted; host units that differ are reported, not replaced. Remaining: the first activation on the VPS (with authorization), after recording the 2026-10-13 release as current.
16. Deploy the remaining accepted local changes to the VPS only when authorized; verify source/input/configuration/image identity and the bounded release gate using the local-to-VPS equivalence procedure, preserving existing state and separate PAPER/private accounts.
17. Finish VPS log acceptance: actual UTC rollover, today/yesterday retention, 50 MiB trimming, recreation and exclusion of secrets. Installation, append/receiver restart, cleanup and boot recovery already passed; durable trading/message/audit growth requires separate planning.
18. Periodic reconciliation done locally 2026-10-14 (`--periodic-reconciliation`, on in the chain): ExecutionState re-checks the exchange hourly 01:00-23:00 UTC and at 23:50, besides startup; size-relative quantity tolerance. Remaining: deploy and see the first scheduled checks on the VPS. Complete VPS Infrastructure/dashboard and operational recovery acceptance: runtime-owned responsiveness/heartbeat, synchronized host clock, stale data, NATS/database failures, duplicate handling, reconciliation, backup/restore, unattended reconnect and off-host retention. Local bounded checks do not complete destination-host acceptance.
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
shadow acceptance. Existing Hyperliquid dry-run logs raw proposed quantities with
`venue_rules_applied=false` and `submitted=false`; it does not provide a complete
shadow harness. Keep the VPS and all real-capital execution unchanged.

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
