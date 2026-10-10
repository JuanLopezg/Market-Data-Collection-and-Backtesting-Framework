# Remaining implementation and acceptance roadmap

Updated: 2026-10-10. This guide owns the ordered remaining work and user requirements.
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

Completed preparation is recorded in CURRENT_STATE.md rather than listed as pending:
research migration, generated navigation, local charts/timeframes, Pipeline/Risk,
external process/NTP observations, bounded browser/backup/connectivity checks,
Telegram delivery and warning-only Kraken account monitoring, coverage analysis,
and the initial VPS PAPER activity study/fast comparison. Multi-day and private
acceptance remain separate. Mobile access was cancelled; the user closed the four
historical differences as final-position closing conventions and RealTest RSI
rounding. Preserve raw differences without claiming exact full-history parity.

Account policy remains warning-only: net venue margin equity below USD 50,
available USD below 10% of equity, daily equity decline of at least 10% versus the
preceding UTC day's last observed equity, and gross confirmed exposure above 5x.
Do not add automatic stops/closures or new opening-order limits. Preserve existing
10% equal-weight sizing and the 1.5x gross strategy constraint. Daily Telegram
holdings already include their USD notional as a percentage of snapshot equity.

1. Observe several UTC daily cycles of the deployed 50/40 PAPER study; verify signals, positions, cash/equity deltas, collector freshness and restart deduplication against the aligned CURRENT fast baseline. Keep close-versus-open sizing differences visible and preserve original 80/70 profiles/state.
2. Gather and implement the user's forthcoming dashboard adjustments once specified; maintain this as a separate pending task without inventing UI requirements.
3. Agree on replacement logs with the user before implementation: content, format, grouping, location, retention and size. Keep the current today/yesterday, 50 MiB per file policy until an explicit replacement is agreed.
4. Define one configurable runtime root and readable data/log/report/mount paths; migrate durable state safely, preserve credentials and distinguish host/container paths. Validate locally first.
5. Confirm Kraken's currently supported private test environment from authoritative sources; the legacy demo endpoint redirected and its guide announces decommissioning. Never fall back to live submission.
6. Implement and validate Kraken private integration in that confirmed test environment: authentication, balances, orders/cancels, partial fills, idempotency, timestamp ordering, restart and reconciliation. Spain/Futures eligibility and READ_ONLY account inspection are accepted, not private execution readiness.
7. Automatically skip intended perpetuals unavailable on the selected exchange, record the reason and continue eligible positions; never substitute spot, another venue or an unsupported product. Distinguish transport failure from an unsupported market.
8. Complete funded Kraken Multi-M BTC/USD collateral economics: valuation/haircuts, margin, USD settlement, conversion/interest and liquidation-risk reporting. Preserve the agreed warning-only account policy; do not automatically sell BTC or rebalance reserves.
9. Account for actual trading commissions and perpetual funding payments/receipts in replay/live, with explicit timestamps, currency, sign and attribution. Keep estimates distinct from confirmed costs; missing funding is unavailable, not zero, and costs must not be counted twice.
10. Add separate Commission and Funding columns to the dashboard Trades view, showing USD costs/receipts and net PnL after both. Gross trade PnL may also be shown; the user accepts net PnL alone. Keep Perpetuals/Costs totals consistent with the underlying ledger and disclose unavailable or unattributed costs.
11. Complete runtime-owned cost basis, realized/unrealized PnL and durable balance/equity history, including collateral and funding effects; verify trade totals reconcile to account-level performance.
12. Verify Manual Control preview, authorization and audit; after private integration is ready, test submission through normal risk/execution, reconciliation and recovery in the confirmed test environment.
13. Extend Live vs. Expected to actual venue execution, costs/funding, skipped markets and collateral constraints using aligned simplest CURRENT PureRSI baseline inputs/settings/dates. Preserve the original raw-volume reference and explain differences instead of forcing parity.
14. Install persistent Kraken read-only monitoring and Telegram delivery services with startup/restart, deduplication, daily summaries and bounded observation/receipt retention. Current private monitoring is local; moving it to the VPS needs separate authorization.
15. Formalize versioned Docker-image archive releases and rollback: image IDs/checksums, required Compose/configuration/host units, durable schema compatibility and previous-image restoration. Image export/load already worked for the study; routine VPS builds/repository transfers should not be required.
16. Deploy the remaining accepted local changes to the VPS only when authorized; verify source/input/configuration/image identity and the bounded release gate using the local-to-VPS equivalence procedure, preserving existing state and separate PAPER/private accounts.
17. Finish VPS log acceptance: actual UTC rollover, today/yesterday retention, 50 MiB trimming, recreation and exclusion of secrets. Installation, append/receiver restart, cleanup and boot recovery already passed; durable trading/message/audit growth requires separate planning.
18. Complete VPS Infrastructure/dashboard and operational recovery acceptance: runtime-owned responsiveness/heartbeat, synchronized host clock, stale data, NATS/database failures, duplicate handling, reconciliation, backup/restore, unattended reconnect and off-host retention. Local bounded checks do not complete destination-host acceptance.
19. Measure complete-backtest CPU/RAM locally and on the VPS, then shadow/live resource use with duration, peak/typical usage and container attribution. Verify disk growth and keep an initial 15 GiB free-space target; do not transfer the whole research collection/build cache.
20. Accept selected-venue forward shadow across several daily cycles and faults, retaining actual market/rule/price observations and the no-submit boundary. Public/PAPER observations do not accept private fills, margin or reconciliation; complete security, monitoring and recovery readiness before capital.
21. Refresh the Binance top-50/25-completed-day quote-turnover coverage analysis before final exchange integration/selection. Previous snapshot: Kraken 38/50, Hyperliquid main 36/50, with HIP-3 37/50. Kraken Multi-M with mixed BTC/USD is the user's current choice; keep public coverage separate from account eligibility.
22. Clarify the incomplete optional exchange-selection note (`and if its s...`) if the user resumes it; do not invent a criterion or block unrelated work.
23. Run a first explicitly authorized real-money test with a small amount on the selected venue after prerequisites pass; monitor orders/fills, funding, margin, alerts, resources and reconciliation. No real-money submission is authorized by this roadmap.
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
   margin/reconciliation behavior; demo/private lifecycle tests remain separate.

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
artifacts and credentials excluded from Git. The activity study already used image export/load successfully. Formal release and
rollback acceptance remains pending; preserve the running PAPER deployment while
observing daily cycles until a further rollout is authorized.

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
