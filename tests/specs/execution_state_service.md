# Execution-state service: accounting rows, position stops, Manual Control files, Binance prices

Scope: the internal library of `live_trading/execution_state_service/src` linked as
`execution_state_core_dep` (everything except `main()`):

| Part | File | What the trading system needs from it |
| --- | --- | --- |
| Daily equity rows | `equity_history.*` | the account at close(T), rebuilt from the fills, funding and venue cash movements |
| Position stops | `position_stops.*` | one resting reduce-only SELL stop per held long, (1 - f) x latest close |
| Manual Control files | `manual_requests.*` | dashboard request files: validation, processed record, re-sync marker, limits, time window |
| Binance last prices | `binance_prices.*` | current price per USDT perpetual for sizing a manual rebalance |

Sources: `live_trading/README.md` ("Accounting projection", "Manual Control"),
`deploy/chain/README.md` ("Manual Control", "Stop-loss"), `docs/codex/STOP_LOSS_DESIGN.md`,
`docs/codex/ARCHITECTURE_FULL.md` ("Service trading"), the headers' comments, and
`lib/src/account/{account,position_ledger}.h`.

Units: quantities in source units (Binance symbols), prices and cash in USD. Business days
are `YYYYMMDD`; replay fill timestamps carry one extra phase digit (`YYYYMMDDp`).

---

## Part A. Daily equity rows (`equity_history.*`)

A day's row is the account **at close(T)**: after the fills of T's open execution, before
those of T+1's open, every nonzero position marked at its close(T). Fills, funding and venue
cash movements (interest, capital flows) are the authority; rows are derived from them.

### A1 Business day of a timestamp

`tradingDayOf(20261014) = 20261014` (live); `tradingDayOf(202610143) = 20261014` (replay,
phase digit 3 dropped).

### A2 Marking: equity = cash + sum(signed quantity x close(T))

cash 4,000; BTC +0.1 at close 60,000 (= +6,000); ETH -1.0 at close 2,500 (= -2,500):
`positions_value = 3,500`, `equity = 7,500`. A short subtracts its marked value. A zero
quantity needs no close. Collateral (BTC after haircut) is recorded beside the equity and is
**not** part of it: with `collateral_usd = 500` equity is still 7,500.

### A3 Ledger totals on the row

`realized_gross_pnl`, `fees`, `funding` and `open_positions` are the position ledger's totals
at that close (all positions, open and closed).

### A4 Interest and capital flows up to the day, recorded beside the equity

Movements (oldest first) dated on or before the row's day are totalled per kind:
interest -1.5 on 20261012 and a deposit (+1,000 capital flow) on 20261013, interest -0.5 on
20261015. Row of 20261013: `interest = -1.5`, `capital_flows = 1,000`. Row of 20261015:
`interest = -2.0`, `capital_flows = 1,000`. Nothing dated after the row's day counts
(no look-ahead). The movements are already in `cash`; the row does not add them to equity
again.

### A5 Rebuild: one row per day from the first fill day to the last completed day

`equityDaysFromFills` walks every calendar day (crypto trades every day) from the business
day of the first fill to `lastDay`. Worked example (initial cash 10,000):

| Day | Event | Cash after | BTC held | close(T) | positions_value | equity |
| --- | --- | --- | --- | --- | --- | --- |
| 20261012 | buy 0.1 BTC at 60,000, fee 3 | 3,997 | 0.10 | 61,000 | 6,100 | 10,097 |
| 20261013 | sell 0.05 BTC at 62,500, fee 1.25 | 7,120.75 | 0.05 | 62,000 | 3,100 | 10,220.75 |
| 20261014 | nothing | 7,120.75 | 0.05 | 59,000 | 2,950 | 10,070.75 |

Ledger on 20261013: realized gross = (62,500 - 60,000) x 0.05 = 125, fees = 4.25, one open
position. Row dates 20261012, 20261013, 20261014 in this order. A month boundary continues
with the next calendar day (20261031 -> 20261101).

### A6 Timing: a row holds only what is dated up to its day

A fill of T+1's open belongs to T+1's row, never to T's (in the example the 20261013 sale does
not appear in the 20261012 row). Funding, interest and capital flows follow the same rule.
`closesFor(day, coins)` is asked for close(day) of exactly the coins held nonzero at that
close (BTC on all three days; a coin closed to flat is not asked for).

### A7 Replay timestamps

A fill stamped `202610121` (replay, phase 1) belongs to day 20261012 and its row.

### A8 Existing rows are kept; the replay still runs through them

Days already in `existing` produce no new row (the first row of a day wins: the stored row
is never replaced), but their fills are still applied: with 20261013 existing, the rebuild
returns 20261012 and 20261014, and 20261014 is exactly the row of A5 (cash 7,120.75,
equity 10,070.75).

### A9 Days without closes are left out

When `closesFor` returns nothing for 20261013, the result is 20261012 and 20261014 (A5
values). No row is invented with stale or missing prices.

### A10 No fills, or lastDay before the first fill: no rows

### A11 Funding and venue cash movements move cash on their day

Funding -2 on 20261013 (BTC) and a +1,000 deposit on 20261014 (A5 example): the 20261013 row
has cash 7,118.75 and `funding = -2`; the 20261014 row has cash 8,118.75,
`capital_flows = 1,000`, equity 8,118.75 + 2,950 = 11,068.75.

### A12 Economic invariant of every row (assumed, from the ledger rules)

`equity - initial cash - capital_flows - interest = realized_gross_pnl - fees + funding +
unrealized PnL of the open positions at close(T)`.
Funding paid on the day a position is opened is part of that day's funding total: buy 0.1 BTC
at 60,000 (fee 3) on 20261012 and funding -2 on 20261012 -> cash 3,995, close 61,000, equity
10,095; 95 = 0 - 3 + (-2) + 100, so the row's `funding` is -2. (The venue reports funding by
business day; a daily timestamp cannot say whether it came before or after the 00:00 open fill.
A position opened at 00:00 is the one that is funded later that day.)

### A13 Duplicate fills are applied once (assumed)

The fill audit trail is keyed by fill id; a fill id that appears twice in the input
(redelivery) changes cash and positions once, as the position ledger does.

### A14 `fillsReachAccount`: the fills explain the live account

True when replaying fills, funding and movements from the initial cash ends at the live cash
(relative tolerance 1e-6) and the live positions. Initial 10,000, buy 0.1 BTC at 60,000 fee 3,
funding -2, interest -1 -> cash 3,994, BTC 0.1. A live account with cash 3,994 and BTC 0.1 is
reached; cash 3,900, or BTC 0.2, is not. A live account that also holds 1 ETH that no fill
explains is **not** reached (assumed: "the state came from another source").

---

## Part B. Position stops (`position_stops.*`)

`planPositionStops(positions, orders, latestCloses, fraction, at, nextOrderId)` is pure: it
returns the stops to submit and cancel.

### B1 A held long gets one stop: SELL the whole position at (1 - f) x latest close

Strategy 1 holds 0.5 BTC, close 60,000, f = 0.30: submit one order, SELL 0.5 BTC,
`position_stop_price = 42,000`, `reference_price = 60,000` (the close it was made from),
`created_at = active_from = at`, order id = `nextOrderId`; `next_order_id` = nextOrderId + 1.
It is a valid position stop (`validateConditional` passes, no `fill_at_reference_price`).

### B2 Nothing to do when the wanted stop rests

A working stop with the same quantity and price (accepted or still submitted) -> no submit,
no cancel.

### B3 Moved at every close, up or down (no ratchet)

Close 60,000 -> 50,000 with an accepted 42,000 stop: submit 0.5 BTC at 35,000. Close
60,000 -> 70,000: submit at 49,000. The old stop is **not** cancelled in the same plan.

### B4 Replace-then-cancel: the old stop is cancelled only once the new one is accepted

Old stop #10 (Accepted, 42,000) and new stop #11 (Submitted, 35,000, wanted): no change.
When #11 is Accepted (or PartiallyFilled): cancel #10 only. The position is never without a
stop.

### B5 Resized when the position changes

Position 0.5 -> 0.8 BTC (same close 60,000) with an accepted 0.5 stop: submit 0.8 at 42,000.

### B6 A flat position has its stops cancelled

No position (or quantity 0) with working stop #10: cancel #10. Terminal stops (Filled,
Canceled, Rejected) and stops already being cancelled (`cancel_requested`) are not cancelled
again. No new stop is submitted.

### B7 Shorts get no stop

Strategy 1 short -0.5 BTC: nothing submitted.

### B8 Missing close: no change, reported

Long 0.5 BTC with no close (or a non-positive close): `missing_closes = {BTCUSDT}`, no
submit, and its existing stop is not cancelled.

### B9 A rejected stop is not resubmitted for the same price and size

Newest stop #12 Rejected at 42,000 for 0.5 BTC and the wanted stop is the same:
`rejected_stops = {12}`, no submit. If the wanted price changes (new close), a new stop is
submitted.

### B10 Per strategy

Strategies 1 and 2 both hold BTC (0.5 and 0.2): two stops, one per strategy, each for its
own quantity, both at (1 - f) x close.

### B11 Deterministic and restart-safe

The same inputs give the same plan (same ids, sides, quantities, prices); ids are
consecutive from `nextOrderId`.

### B12 Invalid inputs are refused

`fraction` must be in (0, 1) and finite; `at` and `nextOrderId` non-zero: otherwise
`std::invalid_argument`.

---

## Part C. Manual Control request files (`manual_requests.*`)

Folder layout: `<dir>/requests/<queued ns>-<id>.request` (dashboard),
`<dir>/processed/<same name>` (outcome), `<dir>/resync_state`.

### C1 The folder needs `requests/` and `processed/` (else `std::invalid_argument`)

### C2 Oldest first

`nextRequest()` returns the request whose name sorts first (the name starts with the queue
time in ns): with `1000-manual-aaaaaaaa.request` and `0999-manual-bbbbbbbb.request` it returns
the 0999 file. Other files (`.tmp`, no extension) are ignored. Empty folder -> nothing.

### C3 A valid request is read completely

```
MANUAL_REQUEST_V1
request_id=manual-0123456789abcdef
actor=operator / OPERATOR
request_hash=sha256:abc
created_at=2026-10-14T12:00:00Z
target=BTCUSDT,0.400000000000
target=ETHUSDT,0.250000000000
```
-> id, actor, hash, created_at as written; weights {BTCUSDT: 0.40, ETHUSDT: 0.25}
(fractions of equity; the remaining 0.35 stays in cash); all 7 lines kept in order for the
processed record. CRLF line ends and blank lines are tolerated. A request with no target
line is valid: everything goes to cash (CASH is optional and held assets missing from the
file are closed).

### C4 A malformed request is refused with `std::invalid_argument` and a reason

Wrong or missing header, a line without `=`, an unknown field, a request id not matching
`manual-<8..64 lowercase hex>`, a missing actor/hash/created_at, an asset that is not
2-30 upper-case letters/digits, a negative, non-numeric, NaN, out-of-range (`1e400`) or
trailing-garbage weight (`0.4x`), a target without a weight, the same asset twice, more than
100 targets, an empty file, a file over 64 KB. (Every one of them must end as a REJECTED
"Invalid request" record, which `main()` writes for `std::invalid_argument`.)

### C5 The processed record is written before the request is removed

`finish(file, lines, outcome)` writes `processed/<same name>` with the request lines, then
`result=EXECUTED|REJECTED`, `detail=` (one line: newlines replaced), `processed_at=`,
`execution_day=` (only when set) and `orders=FIRST-LAST` (only when orders were sent); then the
request file is gone and `alreadyProcessed(file)` is true, so a redelivered copy is never
executed again. A rejection has no `orders=` line. The record is readable by owner and group
only (not world-readable).

### C6 Re-sync marker for the next daily snapshot

No marker -> `resyncForDay(d)` is false. After `markResyncPending()`, the first day asked
(20261015) gets true and the marker binds to it: asking 20261015 again (redelivered snapshot)
is still true, 20261016 is false.

### C7 Limits are the strictest strategy's

Strategy 1: allocation 0.6, max_gross 1.5, max_asset 0.5; strategy 2: allocation 0.4,
max_gross 1.0, max_asset 0.8 -> `max_gross_leverage = 1.0`, `max_asset_weight = 0.5`,
allocations {1: 0.6, 2: 0.4}. A configured strategy missing from the file, a non-positive
limit, a negative allocation -> `std::invalid_argument`; an unreadable file -> error.
The production profile `config/portfolio/pure_rsi_equal_weight.json` loads for strategy 1
(allocation 1.0, limits 1.5 / 1.5).

### C8 No manual orders from 23:45 to 00:15 UTC

23:44:59 -> allowed; 23:45:00, 00:00, 00:14:59 -> inside the window; 00:15:00, 12:00 ->
allowed.

### C9 `created_at` is RFC 3339 UTC seconds, exactly

`2026-10-14T12:00:00Z` -> 2026-10-14 12:00:00 UTC. Any other shape -> nothing:
`2026-10-14 12:00:00`, `2026-10-14T12:00:00+02:00`, `garbage`, empty, and (assumed)
trailing text such as `2026-10-14T12:00:00Zjunk`.

---

## Part D. Binance last prices (`binance_prices.*`)

No real network in tests: a loopback HTTP server stands in for Binance.

### D1 The base URL is required; trailing slashes are ignored

Empty or `"///"` -> `std::invalid_argument`. The request goes to `<base>/fapi/v1/ticker/price`.

### D2 Parsing the answer

`[{"symbol":"BTCUSDT","price":"62000.50","time":1760000000000}, ...]` -> BTCUSDT
{62,000.50, 1760000000000}. Rows with a zero or negative price are left out (no price is
better than a wrong one); a row without `time` gets time 0.

### D3 Failures throw `std::runtime_error`

HTTP status other than 200, an answer that is not a list, an answer that is not JSON, or no
connection -> `std::runtime_error` (header contract), never a partial result.

---

## Findings

Run 2026-10-19: 80 tests, 74 pass, 6 disabled as findings (code disagrees with this spec).

1. **A12 entry-day funding is dropped from the row's funding total**
   (`DISABLED_A12_FundingOnTheEntryDayIsInTheRowFundingTotal`). `equityDaysFromFills` applies
   a day's funding before that day's fills; funding dated on the day a position opens finds no
   open position, and `PositionLedger::applyFunding` ignores it. Evidence: buy 0.1 BTC at 60,000
   (fee 3) and funding -2 on 20261012: cash 3,995 and equity 10,095 are right, but
   `funding = 0` instead of -2, so equity - initial (95) != gross - fees + funding + unrealized
   (97). The same ordering is used by `PositionLedger::rebuild`. With daily cost timestamps
   the order of a funding payment and a same-day fill is not known; a funding payment that finds
   no position is lost from the PnL breakdown while cash keeps it.
2. **A13 a repeated fill id is applied twice to cash/positions**
   (`DISABLED_A13_DuplicateFillIdIsAppliedOnce`). `Account::applyFill` does not de-duplicate
   (its comment leaves that to the caller) while the ledger does, so a duplicated fill row gives
   cash -2,006 instead of 3,997 and equity 10,194 instead of 10,097 on 20261012. Low risk while
   the input comes from `trading_fills` keyed by fill id; the rows are inconsistent if it does not.
3. **A14 `fillsReachAccount` ignores live positions the fills never had**
   (`DISABLED_A14_LivePositionNoFillExplainsIsNotReached`). Only the coins of the replayed
   account are compared; a live account with cash 3,994, BTC 0.1 **and 1 ETH** is reported as
   reached (`true`). Then a history rebuilt from fills that do not explain the account would be
   written as if it did.
4. **C4 an out-of-range weight is not an `std::invalid_argument`**
   (`DISABLED_C4_OutOfRangeWeightIsAnInvalidRequest`). `target=SOLUSDT,1e400` makes `std::stod`
   throw `std::out_of_range` ("stod"). The request is still refused, but `main()` records it as
   `Failed: stod` with an ALERT instead of a readable `Invalid request` rejection.
5. **C9 trailing text after `created_at` is accepted**
   (`DISABLED_C9_OffsetOrTrailingTextGivesNothing`). `parseUtcSeconds("2026-10-14T12:00:00Zjunk")`
   returns 12:00:00 UTC; `std::get_time` stops after `Z` and the rest is not checked. Low impact
   (the dashboard writes the field); the `+02:00` offset form is correctly refused.
6. **D3 a non-JSON Binance answer escapes as `nlohmann::json::parse_error`**
   (`DISABLED_D3_AnswerThatIsNotJsonThrowsRuntimeError`), not the `std::runtime_error` the
   header promises. `main()` catches `std::exception`, so a manual request is still rejected
   with the reason; only the documented contract is broken.

### Fixed (2026-10-19, orchestrator)

- A12: within a business day, `equityDaysFromFills` applies the day's fills (at its open)
  before the day's funding, and `PositionLedger::applyFunding` gives funding that finds no open
  position to that coin's most recently closed position. `PositionLedger::rebuild` applies
  same-time funding before the fills when the asset already has a position, and after them
  when it has none yet (2026-10-23: live timestamps are whole days, so XRP's opening-day funding
  was missing from its closed-position row; accounting R12).
- A13: `equityDaysFromFills` applies a repeated fill ID once to the account too.
- A14: `fillsReachAccount` fails when the live account holds a position the fills never had.
- C4: any unparsable or out-of-range weight is an `invalid_argument` ("invalid weight").
- C9: `parseUtcSeconds` refuses text after the `Z`.
- D3: a non-JSON Binance answer throws `std::runtime_error` ("not JSON").

The corresponding tests are enabled again with their original expectations.
