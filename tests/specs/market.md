# Market behaviour specification

Purpose: supply faithful, completed market observations to indicators, strategy, risk
and close-time planning. A different observation can change selection, position size
or PnL, so missing data must remain missing and future observations must stay hidden.

Sources, in priority order: `docs/codex/ARCHITECTURE_FULL.md` (market domains,
quote turnover, close/open timing), `lib/README.md` (optional quote volume and
reference prices), public `data_types.h`, `canonical_market_data_reader.h`,
`rolling_market_state.h`, `price_snapshot.h`, and `market_messages.h`. SQLite
fixtures use the canonical table layout; no service, network or external dataset is
needed. Requirements not explicitly settled by these contracts are marked (assumed).

## Rules and worked examples

R1. Raw-to-market conversion preserves every supplied bar field and asset/date key.
It changes orientation from asset/date to date/asset, without deriving prices,
indicators or synthetic bars. Example: BTC on 20240301 has open 100, high 120,
low 90, close 110, base volume 3 and quote volume 321. Every value is identical in
the resulting BarData; ETH on that day remains its own independent bar. Empty raw
history gives empty market history, and a missing ETH date stays absent.

R2. Quote volume is actual source turnover, independent of base volume. Missing
quote volume is NaN, including old SQLite schemas without the column and new rows
with SQL NULL. It is never estimated or filled from another date. Example: base
volume 3 and close 110 do not imply quote volume 330; supplied 321 stays 321 and
missing stays NaN. Zero supplied quote volume is valid and distinct from missing.
At ingestion boundaries, supplied negative or infinite quote volume must be rejected
(assumed for the canonical reader's handling of corrupt persisted rows). Conversion
is a lossless reshape, not an ingestion validator.

R3. Bar numbers count an asset's observed bars, starting at 1, in chronological
order. Calendar gaps and other assets do not increment that count. Example: BTC
present on Feb 28, Feb 29 and Mar 1 gets 1, 2, 3; ETH first present on Feb 29 and
next on Mar 1 gets 1, 2. If ETH is missing Feb 29, its Mar 1 bar is still its next
observed bar, not a synthetic intervening observation. A bounded strategy window's
current bar number counts only that asset's loaded rows. Portfolio history is used
for price observations; its bar-number convention is outside this specification.

R4. Rolling updates publish a complete cross-sectional slice atomically. Invalid
timestamp zero, empty slices, empty symbols, duplicate symbols, nonfinite or
nonpositive OHLC, negative/nonfinite base volume, and invalid supplied quote volume
are rejected. Zero base volume is valid. OHLC geometry must also be valid:
low <= min(open, close) <= max(open, close) <= high (assumed for rolling ingestion).
Example: open 100, close 110, high 105, low 90 is rejected. If the second of two
bars is invalid, neither bar is visible, the latest timestamp and counts remain
unchanged, and a corrected retry can succeed once. Canonical full-bar reads must
also reject invalid OHLC/base volume rather than passing it to risk or strategy
(assumed error policy); exception type for corrupt persisted observations is not
part of the contract.

R5. Identical redelivery of the latest completed slice returns false and changes
nothing. Comparison is by asset and complete bar payload, not vector order or NaN
equality. Example: replay BTC/ETH at T with their order reversed and absent quote
volume: two assets and one bar each remain, and the next BTC bar is number 2.
A changed close, quote turnover, quote availability or asset set at the same T is
a conflict, throws, and preserves the original history. Example: close 110 cannot
be overwritten by 111, or quote 321 by 322, through redelivery.

R6. Released rolling history advances strictly by completed slice timestamps.
Out-of-order older slices throw, including an identical older slice after a newer
one has been accepted. Example: append Feb 29, then Mar 1; re-appending Feb 29 is
rejected and Mar 1 remains the frontier. Before Mar 2 is released, no Mar 2 bar or
price is visible; missing assets on a released date are not forward-filled. Existing
rolling history is not an as-of query API: callers evaluate T before appending later
slices. Reconstructing a new RollingMarketState by replaying the accepted prefix,
then continuing, must reproduce uninterrupted history and bar counts. Rolling state
has no durable checkpoint API; replay is the restart mechanism under test.

R7. Point-in-time price snapshots require finite positive prices and fail on missing
assets/dates rather than falling back. Example: a bar with open 100 and close 110
gives open reference 100 and close reference 110. A 110 USD target would resolve to
1 unit at close, but 1.1 units at that open; this area reports the two references
without claiming either was filled. Requesting unknown ETH must throw instead of
returning BTC's price or zero. Rejecting replacement of BTC=110 with 0, -1, NaN or
infinity preserves BTC=110 and must not add an invalid new asset (assumed atomic
rejection, consistent with the validating snapshot API). Previously obtained
snapshots remain unchanged after later slice appends.

R8. Canonical reads open an existing database read-only and require both
`ohlcv_data` and `market_volume_rank_daily`. Missing paths are not created or
silently initialized. Example: reading a nonexistent `missing.sqlite` throws and
the path remains absent. Reading an existing database twice, or reopening it after
a simulated reader restart, leaves its content unchanged and gives the same
observations. Empty paths, date zero, zero history length, zero entry top-N and
invalid compact dates for full-bar windows are invalid arguments. An empty explicit
portfolio symbol set is invalid; an empty exact-close request is valid and returns
an empty map for a nonzero date.

R9. A canonical strategy window uses the persisted ranking for exactly T and includes
only ranks <= N in its entry universe. Its raw history additionally includes explicit
held/active symbols for exit evaluation, never unrelated assets. Both bounds are
inclusive calendar days: start = T - (historyDays - 1 days). Example: T=20240301,
historyDays=3 means Feb 28 through Mar 1, crossing leap day. Daily rankings at T are
BTC=1, ETH=2, OTHER=3; HELD is unranked. N=2 plus extra HELD gives 2 entry assets,
3 history assets and 9 history rows when each has all 3 dates. `market_data` contains
only the T cross-section, while `raw_data` contains the warm-up history. N=1 retains
only BTC for entries; requesting BTC again as extra history does not duplicate it.

R10. Future database rows and rankings never influence a window evaluated at T.
Example: BTC closes on Feb 27, Feb 28, Feb 29, Mar 1 and Mar 2 are 90, 100, 105,
110 and 10000. The three-day Mar 1 window contains exactly 100, 105, 110; neither
90 nor 10000 is available. A one-day Mar 1 window contains only 110. Mar 2 may rank
OTHER first; Mar 1 still ranks BTC first. Inserting a future extreme bar after a
read does not change a repeated T read. Missing dates inside the window remain
absent rather than being backfilled or interpolated.

R11. Required symbols must have a bar on the target date. A previous or future bar
cannot substitute. Example: BTC is ranked on Mar 1 but has only Feb 29=105 and
Mar 2=10000: strategy-window loading fails. The same applies to an explicitly held
symbol needed for exit evaluation and every symbol requested for portfolio history.
No available ranking on T is an error for a strategy window; an earlier or future
ranking cannot replace it.

R12. Portfolio history is bounded by the same inclusive calendar-day rule and loads
only the explicit symbols, independently of entry ranking. Example: a Mar 1
three-day request for HELD gives only HELD on Feb 28, Feb 29 and Mar 1, even if HELD
is unranked or all ranking rows are absent. A BTC gap on Feb 29 remains a gap;
Mar 1 must still exist. No future row can enter covariance or volatility inputs.

R13. Exact closing-price reads return only requested completed close(T), with no
fallback and no fill-price interpretation. Example: BTC close(T)=110 and
ETH close(T)=22 give exactly those two values; unrelated HELD=11 is omitted.
If ETH lacks T but has T-1=21 and T+1=2000, the whole request fails. A close of
0, a negative value, NULL or infinity must fail (assumed corrupt-row policy).
Open(T+1)=9000 must never replace close(T)=110.

R14. Restart/catch-up uses persisted ranking availability, bounded by the requested
frontier, rather than the current clock or newest OHLCV row. Example: rankings exist
on 20240229, 20240301 and 20240302. A frontier of 20240228 returns no date;
20240229 returns Feb 29; 20240301 returns Mar 1; 20240303 returns Mar 2. Exact
availability for Feb 28 is false despite OHLCV existing there. Frontier zero is an
invalid argument. Reopening the reader preserves these answers.

## Invariants and timing

- `Timestamp` is YYYYMMDD, never Unix milliseconds; use explicit synthetic daily keys.
- At close(T), all feature/valuation inputs are from dates <= T. Only a released
  completed bar contributes to rolling state; open(T+1) belongs to execution later.
- Base volume and quote turnover retain separate source meanings across conversion,
  SQLite reads and rolling history. Unknown turnover cannot become known by arithmetic.
- Rejected or duplicate slices cannot add bars, advance history counts or overwrite
  accepted observations. Complete cross-sections protect liquidity/ranking decisions.
- Reads, restarts and replay cannot create orders, fills, positions or accounting
  mutations. This area supplies observations to those later stages.

## Findings

The first build passed without compiler warnings in the new tests. The first run
found the three disagreements below; no expected values or specification rules were
changed. Their tests retain their assertions and use the `DISABLED_` prefix.

F1 (R4, rolling OHLC geometry). `RollingMarketState::append` accepts both
`{open=100, high=105, low=90, close=110, volume=3, quote_volume=321}` and
`{open=100, high=120, low=105, close=110, volume=3, quote_volume=321}`. In both
cases, the expected invalid_argument is absent and market history becomes nonempty.
The rolling validator checks signs/finiteness but not enclosure of open/close by
high/low. Retained test:
`RollingMarket.DISABLED_R4_HighAndLowMustEncloseOpenAndClose`. The assumption in
R4 is retained; no higher-priority contract authorizes invalid OHLC geometry.

Fixed (2026-10-19): Reject rolling OHLC bars whose high/low do not enclose open/close before publishing the slice.

F2 (R2, corrupt persisted quote turnover). With BTC's Mar 1 `quote_volume` set to
SQL `-1` or `1e999` (positive infinity), both `loadWindow(20240301,3,1)` and
`loadMarketDataWindow(20240301,3,{"BTC"})` return without throwing: all four
rejection assertions fail. The canonical reader copies quote_volume but its bar
validator does not validate that field. Retained test:
`CanonicalMarket.DISABLED_R2_CorruptSuppliedQuoteTurnoverCannotReachStrategyOrRisk`.
This is the explicitly assumed corrupt-row policy in R2, not a claim that the normal
writer emits these values. Missing/NULL quote volume, legacy schemas and valid zero
turnover pass their separate tests.

Fixed (2026-10-19): Reject supplied negative or nonfinite quote turnover in both canonical full-bar readers.

F3 (R4, corrupt OHLCV error path crashes). The first case of the invalid-row test
sets BTC's Mar 1 high to 105 while close remains 110. Its `loadWindow` call
terminates the process with SIGSEGV (exit 139), rather than throwing an exception.
Source inspection of `canonical_market_data_reader.cpp` shows both full-bar loader
error branches call `sqlite3_finalize(stmt)` before using the SQLite-owned `pair`
and calling `sqlite3_column_int64(stmt,1)` to build the error message. This is an
unsafe use after finalization. The portfolio loader's equivalent branch and the
remaining invalid-row cases were not reached in the crashing run. Retained test:
`CanonicalMarket.DISABLED_R4_InvalidOhlcvRowsCannotReachStrategyOrRisk`.

Fixed (2026-10-19): Copy invalid-row diagnostics before finalizing either canonical reader statement.

Reproduction: build with `ninja -C build -j2`, then run
`ALGOTRADING_SOURCE_ROOT=$PWD build/tests/unit/market/test_unit_market`. The original
run reaches F1, F2, then crashes at F3. To exercise retained regressions individually
after triage, add `--gtest_also_run_disabled_tests` and select the full test name
using `--gtest_filter`. The four R8 tests following F3 in registration order were
run separately with `--gtest_filter=CanonicalMarket.R8_*`; all four passed.

Final verification after disabling the three findings: `ninja -C build -j2` passed
without test-code warnings; `meson test -C build --no-rebuild --suite unit
unit_market --print-errorlogs --verbose` exited 0. GoogleTest reported 55 registered
tests: 52 passed, 3 disabled, no enabled failures (Meson 1/1 executable passed).
