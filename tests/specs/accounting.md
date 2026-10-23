# Accounting behaviour specification

## Purpose and sources

Accounting turns accepted fills into cash and signed positions, maintains an
account-level cost-basis ledger, reconstructs strategy trade campaigns, and reports
historical cash, realized balance and marked equity. It must preserve money across
partial fills, reductions, reversals, fees, funding, duplicates and reconstruction.

Sources: `docs/codex/CONTEXT_FULL.txt`, `docs/codex/ARCHITECTURE_FULL.md` (Timing and
economics, Restart/resume, account-cost boundaries), `lib/README.md` (Backtester,
execution and accounting boundaries), and public headers `account/{account,
position_ledger}.h`, `position/position_state.h`, `analytics/{trade_recorder,
trade_record}.h`, `backtest/{backtest_context,backtester}.h`, `execution/fill.h`,
`strategy/{strategy,strategy_instance}.h`, `sizing/equal_weight_sizer.h`,
`rebalance/entry_exit_only_rebalance_policy.h` and `risk/risk_constraints.h`.

These are cash/filled-position accounts, not leveraged venue margin accounts.
Funding is modeled explicitly by PositionLedger and Account cash movements; it is
not automatically inserted into TradeRecorder or the historical simulator.

Important distinction: PositionLedger averages cost and realizes gains on partial
reductions, while TradeRecorder records whole strategy campaigns and preserves the
first entry and final exit fill prices. Backtest balance uses fully closed campaign
PnL, not ledger partial-realization totals. Tests must not conflate these contracts.

Examples start with USD 1,000, BTC/ETH, strategies 7/9 (Alpha/Beta), and business
dates D1=20260105, D2=20260106, D3=20260107, D4=20260108. All fill IDs are positive
and unique unless testing a repeated delivery. Every expected result below follows
hand arithmetic or a stated invariant. No Internet, storage dataset or wall clock.

## Rules and worked examples

### R1 — Cash and signed quantity follow actual fills

For signed fill q, cash delta = −q×price−commission; position delta = q.
BUY 2 at 100 fee 2 gives cash 798, BTC +2, equity 998 marked at 100.
SELL 2 at 100 fee 2 from flat gives cash 1,198, BTC −2, equity 998.
Exact round trip BUY 2 at 100 fee 2 then SELL 2 at 120 fee 2.40 leaves BTC zero
and cash/equity 1,035.60. Separate ETH holdings must remain unchanged by BTC fills.

### R2 — Marks change equity, never cash or filled quantity

After BUY 2 at 100 fee 2, marking 120 gives equity 798+240=1,038; marking 90
gives 978. For SELL 2 from flat, marking 90 gives 1,198−180=1,018. Missing any
held asset's mark is an error; an empty account needs no marks. Holdings valued
at their fill prices conserve initial equity minus fees, including simultaneous
longs and shorts. No planned order or mark is itself an executed fill.

### R3 — Cash movements and restore are explicit and validated

Venue funding received +3 then paid −1 changes cash 1,000 to 1,002 without units.
Account does not infer funding from marks or deduplicate cash-movement calls.
Restoring cash −50 with BTC −2 and ETH +3 is valid authoritative state; at marks
100/20 equity is −50−200+60=−190. Nonfinite cash/movements/positions and invalid
fill quantity/price/fee reject before mutating the previous valid account state.
Construction requires positive finite initial cash; restart may restore zero or
negative cash. PositionState rejects nonfinite additions and must not retain a
partial update; adding +2 and then −2 removes the asset rather than retaining dust.

### R4 — Account/analytics callers own accepted-fill deduplication

Account and TradeRecorder apply each supplied fill; their caller must deliver each
accepted FillID once. PositionLedger, in contrast, explicitly deduplicates fills.
Example: a deduplicated BUY 2 at 100 fee 2 leaves cash 798 and one campaign with
one fill and commission 2; reconstruction must reproduce those amounts.
Restart reconstructs analytics from the authoritative, deduplicated fill trail;
it must not invent fills from report rows or force-close open positions. Execution
area tests cover duplicate delivery to the account through the owning engine.

### R5 — Ledger adds at weighted average cost

BUY 2 at 100 fee 2 then BUY 2 at 200 fee 4 produces quantity +4, average entry
(200+400)/4=150, cumulative entry value 600, peak quantity 4 and fees 6. A short
SELL 2 at 100 plus SELL 2 at 200 has quantity −4 and the same average 150.
Entry value is total opening/increasing notional over the campaign, not the
remaining position's current mark or a sign-sensitive cash balance.

### R6 — Reductions realize PnL at unchanged average cost

From R5, SELL 1 at 180 fee 1.80 leaves +3 at average 150, realized gross PnL 30,
exit value 180, exit quantity 1, fees 7.80. At mark 170 unrealized PnL is
(170−150)×3=60; cash is 572.20, equity 1,082.20, and
1,082.20−1,000 = realized 30 + unrealized 60 − fees 7.80.
For the short R5 example, BUY 1 at 120 fee 1.20 realizes 30 and leaves −3.
Closing the long remainder with SELL 3 at 140 fee 4.20 realizes −30 more:
gross total 0, fees 12, net −12, cash 988, exit average (180+420)/4=150.
The closed lot has zero quantity, closing fill ID/date, and no open remainder.

### R7 — Adding after a reduction averages only remaining units

BUY 2 at 100, BUY 2 at 200, SELL 1 at 180, then BUY 1 at 210 gives +4 at
(3×150+210)/4=165, realized gross still 30, cumulative entry value 810 and peak
quantity 4. Zero fees in this example isolate cost arithmetic. A partial reduction
must not allow already-closed units to dilute the new average.

### R8 — Reversals close one lot and open the remainder with split fees

BUY 2 at 100 fee 2, then SELL 5 at 120 fee 6 closes the long: gross 40, fees
2+6×2/5=4.40, net 35.60. It opens short −3 at 120 with fee 6×3/5=3.60;
cash is 1,392 and marked equity at 110 is 1,062. Total fees remain 8.
Symmetrically SELL 2 at 100 fee 2 then BUY 5 at 80 fee 4 closes short with gross
40, fees 3.60, net 36.40 and opens long +3 at 80 fee 2.40. Cash 794 and equity
at 90 is 1,064. New lot dates and entry price come from the reversing fill.

### R9 — Ledger aggregates by account asset, independently of strategy

Alpha BUY 2 BTC at 100 fee 2 followed by Beta SELL 2 BTC at 120 fee 2.40 closes
one account BTC lot, gross 40/net 35.60, despite different strategies. An ETH
SELL 3 at 20 fee 0.60 is a separate short lot. Closing BTC must not consume ETH
quantity, costs or funding; account totals sum open and closed lots.

### R10 — Ledger fills are idempotent and invalid payloads do not poison IDs

Deliver BUY 2 at 100 fee 2, ID 101 twice: one +2 lot, fees 2. Repeating the
closing ID cannot create an opposite lot or duplicate closed PnL. A rejected
nonfinite/negative fill with ID 102 must not reserve that ID: corrected BUY 2 at
200 fee 4 must still yield R5's +4 average 150 and fees 6. This expectation follows
the public validate-then-apply contract (atomic invalid-payload rejection assumed).

### R11 — Funding attaches once to the lot held when it occurs

With a BTC lot open, receipt +3 and payment −1 yield funding +2; a repeated named
payment is ignored. Net PnL = gross−fees+funding, so a long closed at gross 40,
fees 4.40 plus funding 3 has net 38.60. Funding on ETH cannot affect BTC.
A payment observed while flat is ignored and must not later attach to a new lot
when the same payment ID is redelivered. Nonfinite funding rejects; corrected
funding with the same ID remains eligible. Empty payment IDs have no specified
retry guarantee; tests use named payments.

### R12 — Rebuild follows timestamp/fill-ID order and funding precedes same-time fills

Audit example: BUY 2 at 100 fee 2 (D1, ID 101), SELL 2 at 120 fee 2.40
(D2, ID 102), SELL 1 at 120 fee 1.20 (D2, ID 103). Funding D0 +9 (the day before)
is before the opening and ignored; D2 +3 belongs to the long held into D2; D3 −1 belongs to the
new short. Closed long gross 40/fees 4.40/funding 3/net 38.60; open short −1 has
fee 1.20/funding −1/net −2.20 before unrealized PnL; total funding 2 and fees 5.60.
Any input permutation yields these same lots, including ties sorted by fill ID.
Same-time funding for an asset with no open position belongs to the position the fills at
that time open (live timestamps are whole days: fills at the open, funding hourly after).
Rebuilding the same trail or duplicate identical audit rows must not duplicate
money. Rebuilt state remains live: applying a new unique fill continues the lot.
Covering the remaining short 1 at 100 fee 1 on D4 realizes gross 20; its total
fees 2.20 and funding −1 give net 16.80. Aggregate gross is then 40+20=60.

### R13 — Trade campaigns use first-entry/final-exit prices, not ledger average

R5's two buys plus R6's reductions form one Alpha/BTC long campaign. Before the
final exit, first entry stays 100, peak size 4, fill count 3, fees 7.80, marked PnL
82.20 at 170, and cumulative *closed* PnL remains zero. Final SELL 3 at 140 gives
one closed campaign entry 100/exit 140, four fills, fees 12 and PnL −12. Start is
D1 and end is the last exit's date. Intermediate reductions and marks cannot rewrite
the entry or final closing fill price; mark snapshots are nonmutating views.
The same partially reduced campaign marked at 100 has PnL
−200−400+180+3×100−7.80 = −127.80, without changing the prior 82.20 report.

### R14 — Campaign shorts and reversals conserve fees and total PnL

SELL 2 at 100 fee 2, BUY 1 at 80 fee 0.80, then BUY 1 at 90 fee 0.90 closes a
short campaign with gross 30, fees 3.70, net 26.30; entry 100, final exit 90.
R8's long-to-short reversal creates a closed long campaign PnL 35.60 and an open
short marked at 110 with PnL 30−3.60=26.40: total 62, matching account equity
1,062−1,000. Campaign fees are split 4.40/3.60, never charged twice. Its short-to-long
mirror gives closed 36.40 plus open marked 90 PnL 30−2.40=27.60, total 64.

### R15 — Campaign identity remains per strategy and asset

Alpha BUY 2 at 100 fee 2 and Beta SELL 2 at 100 fee 2 produce two campaigns, one
Long and one Short, though account quantity nets to zero. At mark 110 their PnLs
are +18 and −22, total −4 matching fees. ETH trades cannot close BTC campaigns.
A later opening after flat starts a new campaign rather than rewriting history.
Closed campaigns require no current price; open campaigns require their own marks.
Invalid fill payloads throw without changing prior campaigns. Clear plus replay of
the same deduplicated fill trail recreates economic records and stable trade IDs.

### R16 — Backtest balances distinguish cash, closed-campaign balance and equity

Balance = initial capital + cumulative fully closed campaign PnL. Equity = cash +
marked signed positions. After R5/R6's partial close with fees: cash 572.20,
balance 1,000, equity 1,082.20 at mark 170. After full close all are 988. Open
entry fees are reflected in equity/cash immediately; balance waits for campaign
closure. Account-history snapshots are value records, not live mutable projections.

### R17 — Synthetic Backtester executes close(T) decisions at open(T+1)

A deterministic signal source requests BTC long at D1 close and flat at D2 close,
using real EqualWeightSizer(0.5), RiskConstraints(1,1), EntryExitOnlyRebalancePolicy,
TradingEngine and SimulatedExchange. Data:

| Day | Open | Close | Close signal |
| --- | ---: | ---: | ---: |
| D1 | 80 | 100 | +1 |
| D2 | 125 | 150 | 0 |
| D3 | 140 | 130 | unchanged |

Initial 1,000 fixes USD 500 at D1. D1 snapshot cash/balance/equity = 1,000.
D2 BUY 500/125=4: cash 500, balance 1,000, equity 1,100. D3 SELL actual 4 at
140: cash/balance/equity 1,060. Trade entry D2/125, exit D3/140, size 4, PnL 60.
D1 open/close must not supply the entry price; D2 close must not supply exit price.
A signal on the last bar has no later open and cannot manufacture a trade.
With 1% commission, D2 fee 5: cash 495/balance 1,000/equity 1,095; D3 fee 5.60:
final cash/balance/equity 1,049.40 and trade commission 10.60/PnL 49.40.

### R18 — Gaps, HOLD, shorts and simultaneous strategies keep actual units

Changing D2 open to 250 changes entry units to 2 (=500/250), cash remains 500,
D2 equity 800 at close 150. At open 50 units are 10, cash 500, close equity 2,000.
For HOLD after long entry at 125, D3 close 130 keeps 4 units/cash 500/equity 1,020,
rather than resizing from the new equity. Short −1 at D1, flat at D2, and D3 open
100 sells 4 at 125 and buys 4 at 100: D2 cash 1,500/equity 900, final 1,100,
closed short PnL 100. Two strategies allocated 50% each with full signal weights
+1/−1 trade +4/−4 at D2 open 125; with 1% fees their account units net zero,
cash/equity 990, attribution remains +4/−4, and two campaigns remain open.

### R19 — Cutoff does not force fills and future changes cannot alter past economics

Truncate R17 after D2 with 1% fees: position +4, cash 495, balance 1,000, equity
1,095, open campaign PnL 95. The end of the data adds no exit fee, forced close or
additional snapshot (the former no-op `closeTrades()` was removed on 2026-10-22). Extending the data by D3 must not alter D1/D2
snapshots. Perturbing D3's open/close/high/low cannot change earlier history.
A Backtester may hold the full data container; the deterministic strategy indexes
only the current date and these tests do not assume the container itself is truncated.
Compatibility trade reports must retain open/closed flags, prices, fees and PnL
from fill-derived TradeRecords, and must never drive account changes.

### R20 — Reject unusable configuration and marks explicitly

Nonpositive/nonfinite initial capital, negative/nonfinite commission, no strategies,
duplicate strategy IDs or allocations totaling more than 100% must reject setup.
Example: allocations 0.6+0.6 are not USD 1,200 spendable capital in a USD 1,000
account. Nonempty synthetic raw data is always supplied; empty-history assertions
are not exercised. Held positions without valid marks cause an error rather than
fabricated zero exposure. No partial cash/position update is valid on bad input.

## Invariants and scope

- Cash delta per accepted fill is signed proceeds minus its fee exactly once.
- Account equity − starting capital equals realized gross + unrealized − fees +
  applied cash funding, for consistently initialized state.
- Ledger and campaign totals sum independent assets; campaign attribution also
  retains strategy identity. Neither may silently lose the remainder of a reversal.
- Reports/marks/targets cannot change authoritative filled units.
- Tests use explicit historical dates, synthetic prices and unique economic IDs.
- Operational fill deduplication/dispatch and durable engine recovery are covered by
  execution/recovery; ledger deduplication and analytics reconstruction are covered here.
- All expected numbers are fixed examples or independently stated invariants;
  no output-dependent expected values, real databases/services or slow visual tests.

## Findings

No findings from step 3. All original assertions passed; no tests, expected values
or specification rules required correction, and no tests were disabled.

Validation (WSL, 2026-10-19): `ninja -C build -j2` compiled and linked successfully
without warnings. The command
`ALGOTRADING_SOURCE_ROOT="$PWD" ALGOTRADING_LOG_QUIET=1 build/tests/unit/accounting/test_unit_accounting --gtest_brief=1`
exited 0: **58 tests total, 58 passed, 0 disabled, 0 failures**. Coverage includes
12 account/position tests, 20 ledger tests, 13 campaign-recorder tests and 13
Backtester/context tests. Only this spec changed during step 3; production code
and test expectations are untouched.
