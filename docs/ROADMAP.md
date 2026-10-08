# Remaining implementation and acceptance roadmap

Updated: 2026-10-08. This guide owns the ordered remaining work and user requirements.
[CURRENT_STATE.md](../CURRENT_STATE.md) owns validation evidence; component guides
and actual source describe implemented behavior. Entries below are pending work,
not claims that a capability is implemented, broken or ready for live capital.

Research runtime migration and generated `.ai/` navigation are complete. Preserve
the accepted Step59 fingerprint and CURRENT economics during future changes.

PAPER liquidity now uses actual completed daily quote turnover in USDT, averaged
over 25 rows, rather than base-unit volume. Its separate profile preserves the
historical reference. Observe the configured future-candle cutover and accumulate
enough new-metric observations before accepting rolling comparisons.

Before renting a VPS, use the prepared [controlled local service campaign](../validation/LOCAL_SERVICE_CAMPAIGN.md)
and the one-command `research/replay.py resources --days 100 --seconds 300`
measurement with dashboard and an offline CPU/RAM HTML report.
It covers bounded pipeline evidence, duplicates, checkpoint/outbox crash barriers,
service/infrastructure recovery, recreation, logs and resource sampling. Record
observed passes and remaining cases in CURRENT_STATE.md; local success does not
complete the VPS-specific entries below or authorize private trading.
The user has deferred the remaining fault/recreation/overnight-retention campaign
to the VPS; keep those acceptance checks outstanding rather than running them now.

## Ordered steps

Preparation prerequisite: local deployment/data/log disk inventory is complete;
see CURRENT_STATE.md and the [disk budget](../deploy/live/README.md#vps-disk-capacity-and-transfer-scope).
The [current-data paper stack](../deploy/paper_trading/README.md) is prepared locally,
including virtual execution and dashboard host/container telemetry.
The first local current-data cycle and dashboard API/browser inspection passed.
The Linux 2-vCPU/2-GB Lightsail VPS now runs PAPER with fresh credentials, a matching
bounded Step59 result, host telemetry, persistent daily logs and automatic startup.
Its actual public-data no-order cycle, boot recovery, receiver restart without
interrupting PAPER and isolated two-fill fixture passed. Multi-day, UTC rollover,
broader fault recovery and capacity acceptance remain pending.
Next observe successive daily cycles and retained logs on that VPS.
A 60 GB disk is a reasonable candidate based on
local sizes; verify actual space/growth overnight, keep an initial 15 GiB free-space
target and finish disk monitoring. Daily host logs now retain today/yesterday with
a 50 MiB cap per file (older diagnostics trimmed); durable
trading/message/audit state needs separate growth/recovery planning. Do not transfer
the whole research data collection or accumulated Docker build cache.

1. Finish VPS daily-log acceptance: observe two-day retention, actual UTC rollover, 50 MiB trimming, recreation and exclusion of secrets. Installation, append/receiver restart, cleanup and boot recovery passed.
2. Extend the accepted VPS source/input, eight-runtime-target build, configuration, packaging and bounded replay comparison using the [equivalence procedure](../deploy/live/README.md#local-to-vps-equivalence-acceptance) when source/settings change; complete broader deployment acceptance separately.
3. Validate deployed NATS/database integration, timestamp ordering, duplicate handling, reconciliation and crash/restart recovery; a local adapter crash/restart fixture is available, while full service/VPS acceptance remains pending.
4. Measure CPU/RAM during a complete backtest locally and on the VPS; retain duration, peak/typical usage and service/container attribution, then repeat during shadow and live trading.
5. Compare Kraken and Hyperliquid perpetual-market coverage for the same top 50 Binance coins ranked by trading volume over the last 25 completed days; save the reproducible universe, matches, unavailable assets, counts and percentages.
6. Complete the unresolved exchange-selection criterion from the original note (`and if its s...`) when clarified; do not invent its meaning or silently treat it as satisfied.
7. Verify account/product eligibility and BTC collateral for perpetuals on the candidate venues; determine required balance, valuation, margin, haircut and liquidation-risk handling before selecting the live configuration.
8. Select the initial venue using the coverage/collateral findings; Kraken is the user's likely first live venue because they already have an account. Hyperliquid is a comparison candidate, not the initial integration priority.
9. Implement and validate the selected venue's private API integration in its supported test/demo environment: authentication, balances, orders, cancellations, partial fills, idempotency, restart and reconciliation.
10. Skip a position automatically when its intended perpetual market is unavailable on the selected venue; record the reason and continue eligible positions without a spot, alternate-venue or unsupported-product fallback.
11. Add perpetual funding payments and receipts alongside trading fees in replay/live accounting, net performance and Perpetuals/Costs reporting, using actual event timestamps and explicit sign/currency handling.
12. Complete runtime-owned cost basis, realized/unrealized PnL and durable balance/equity history, including the selected venue's collateral valuation and funding effects.
13. Improve Overview charts with a labeled Balance/Equity Y-axis, units/currency, hover values and navigation/zoom on both X and Y axes, backed by available verified history.
14. Fix Overview timeframe buttons, add ALL, and validate that every selection filters the intended time window consistently with chart navigation and the selected data provider.
15. Investigate the empty Pipeline section; verify timestamp-aligned source/API/UI joins and make genuine missing, pending or stale evidence understandable rather than fabricating completed cycles.
16. Investigate the empty Risk section and complete persisted sizing, constraint and risk-transform diagnostics; distinguish unavailable evidence from an actual zero-risk result.
17. Accept Infrastructure telemetry on the VPS; local paper CPU/RAM/disk readings, freshness handling, container observations and readability are implemented. Complete business heartbeats and verified clock sync; container liveness does not prove trading readiness.
18. Configure and test Telegram alerts using the existing optional notifier adapter; complete missing alert coverage and verify delivery, retries, deduplication, durable receipts and secret-safe errors.
19. Verify Manual Control preview, authorization and audit now; once private exchange integration is ready, test actual submission through normal risk/execution, reconciliation and recovery in the test/demo environment.
20. Complete and verify Live vs. Expected against the simplest existing PureRSI fast backtest baseline, using aligned inputs, strategy settings, quote-volume universe, sizing, dates and cost assumptions; obtain actual historical quote turnover for the PAPER variant and keep the original raw-volume reference separate. Keep execution/cost differences visible.
21. Review the four outstanding full-history replay differences and complete their explicit acceptance or correction; keep the comparison direct and do not hide mismatches behind a whitelist.
22. Complete paced dashboard/browser acceptance, including charts/timeframes, Pipeline, Risk, Infrastructure, Manual Control and Live vs. Expected for the relevant provider/environment.
23. Validate backups/restoration, alerts and operational failure handling, including loss of messaging/database/venue connectivity and continued trading when the dashboard stops.
24. Run shadow trading on the selected venue and compare intended decisions, eligible markets, expected performance, costs and reconciliation with observed market/exchange evidence.
25. Complete security review and live-capital readiness checks, with explicit risk limits, monitoring, stop conditions and recovery procedures.
26. Start the first explicitly authorized live test with a small amount of money, preferably on Kraken if the prior checks support it; monitor CPU/RAM, orders/fills, funding, margin, alerts and reconciliation.
27. Increase capital gradually only after the small live test meets agreed acceptance criteria.
28. Continue optional strategy research: OOS results, rankings, correlations, portfolio risk, capacity, fees/slippage/funding, gaps and intraday variants; research experiments remain separate from validated strategies.

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
artifacts and credentials excluded from Git. This is explicitly deferred work;
leave the running PAPER deployment unchanged while observing daily cycles.

## Exchange analysis and baseline details

For the coverage study, record the analysis date, exact 25-day window, Binance
market segment, comparable quote-volume normalization, base-asset/pair
deduplication, symbol mappings and data gaps. Use the identical ranked 50-coin
universe for both venue comparisons. Check currently tradable perpetual contracts,
not merely spot listings or a historical catalog entry; keep public market coverage
separate from the user's account/jurisdiction eligibility and BTC-margin support.
Verify venue API, contract and collateral rules from current authoritative sources
when performing that task. No present coverage or collateral-support result is assumed.

The incomplete original exchange note remains open, while the coverage comparison
and explicit BTC-collateral requirement can proceed independently. Do not block
unrelated dashboard/logging work on that missing criterion.

The existing lightweight baseline is `python3 research/replay.py fast` using CURRENT
`Backtester` and validated PureRSI. Reuse this baseline rather than inventing another
engine or requiring the distributed/canonical service stack for the simplest
comparison. The existing Live vs. Expected projection covers market/strategy-input
anomalies; execution/accounting and accepted baseline comparisons need further work.
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
