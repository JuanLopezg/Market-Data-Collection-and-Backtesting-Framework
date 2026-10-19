# In-process trading and replay runtime behaviour

## Purpose, sources and fixtures

Connect completed market data to real strategy, risk, sizing, planning, execution,
fills and account state without making a decision into a fill. A completed-day
restart must reproduce the uninterrupted economic result. Manual allocation must
pass the same risk and venue boundaries.

Sources: `docs/codex/CONTEXT_FULL.txt`, `docs/codex/ARCHITECTURE_FULL.md` (canonical
timing and restart), `lib/README.md`, the public `decision_engine.h`,
`trading_engine.h`, `replay_runtime.h`, `manual_trading.h`, and the public strategy,
planning, accounting and MOCK contracts they connect. Assumptions not explicitly
specified there are marked `(assumed)`. No production code is changed.

Tests use real PureRSI(2), entry > 60, exit < 40; EqualWeightSizer(.25),
EntryExitOnlyRebalancePolicy and RiskConstraints(1,1), unless stated otherwise.
Initial cash is USD 1000. The single configured strategy is ID 7 with allocation 1.
No Internet, database service or historical storage is used. SQLite and the canonical
MOCK journal live in per-test temporary directories. SimulatedExchange is used for
the in-process chain; FakeExchange represents only an asynchronous external venue.

Economic event keys explicitly distinguish each phase, following canonical replay:
open(day) = YYYYMMDD*10+1; manual midpoint = YYYYMMDD*10+5;
close(day) = YYYYMMDD*10+9. They are synthetic event labels, not Unix timestamps.
No test sleeps or reads wall time to choose a trade.

| Day in January 2026 | Open | Close | Known only after this close |
| --- | ---: | ---: | --- |
| 1 | 100 | 100 | RSI unavailable; no entry. |
| 2 | 105 | 110 | Only one return; RSI(2) unavailable. |
| 3 | 115 | 120 | Gains 10,10; average gain 10/loss 0 -> RSI 100, enter. |
| 4 | 125 | 100 | Wilder gain 5/loss 10 -> RSI 100-100/(1+.5) = 33 1/3, exit. |
| 5 | 150 | 90 | Wilder gain 2.5/loss 10 -> RSI 20, remain flat. |

Each complete OHLCV bar has high=max(open,close)+5, low=min(open,close)-5 and
volume 10000. Variants explicitly name changed prices. At close(3), capital 1000
times weight .25 fixes USD 250; open(4)=125 resolves 2 units. Buy 2 at 125 -> cash
750/position 2. At close(4)=100, equity is 750+200=950. Exit at open(5)=150 sells
exactly 2 -> cash/equity 1050, gross/net closed PnL 50 with zero fees.

## Decision and in-process trading rules

**R1 — Completed-close signals do not use future bars.** Warm-up produces no entry.
The first approved entry is close(3), even if open(4), close(4) or the final day is
already present in a caller's precomputed history. Example: replacing close(4) 100
by 10000 cannot change close(3)'s +1 signal, .25 target or USD 250 intent. The
DecisionEngine owns no fill/account mutation; supplied filled positions remain intact.

**R2 — Capture approved capital/weights, not executable units.** Allocation, asset
cap and gross cap apply before execution. Example: equity 1000, allocation .5 and
weight .25 -> capital 500, exposure 125; an asset cap .10 instead gives exposure
100 with allocation 1. TradingEngine's extraCapital is sizing-only: cash 1000 plus
collateral 500 gives reference capital 1500 and target 375, hence 3 units at open
125, while cash stays 1000 until a fill and then becomes 625. Negative/nonfinite
extra capital is rejected before new plans/orders are created.

**R3 — HOLD preserves filled units; FLAT closes the actual position.** At a later
close, a continuing +1 signal with 2 held units is HOLD even if equity/price changed;
it must not buy back to .25 weight. An exit closes the held 2, not 950*.25/150.
Short signs also matter: a restored short -2 with cash 1250 closes with Buy 2 at
100 -> cash 1050/flat, never Sell 2. Missing held assets in the entry universe do
not erase them: split signal/sizing input still supplies the full held close and
may produce their exit. All strategy virtual holdings sum to account holdings.

**R4 — Pending intent tracks the latest completed decision.** An unexecuted plan
for one strategy is replaced rather than appended when a newer actionable close
arrives. `(assumed)` If the newer signal says flat while holdings are already zero,
the obsolete entry must be removed even though the new rebalance map is empty.
Example: entry at close(3) is not executed; close(4) exits the signal while account
remains flat -> no stale USD 250 entry is left for open(5). Clearing pending intent
does not clear persistent strategy signals.

**R5 — Decisions and submissions spend nothing; actual next-open fills do.** Closing
day 3 leaves cash 1000/position 0/no fills. Planning/submitting at open(4) alone
still leaves cash 1000/position 0. Processing the accepted venue fill then gives
cash 750/position 2. The base round trip finishes with two orders/two fills/PnL 50.
`(assumed for the generic TradingEngine market path)` Resolving an entry at its
decision close or an earlier timestamp must reject rather than enable a same-close
fill; ReplayRuntime explicitly requires open > decision close.

**R6 — Fills and fees drive cash and analytics exactly once.** With a 1% commission,
entry fee=2*125*.01=2.5, exit fee=2*150*.01=3; final cash 1044.5 and closed PnL
44.5 (=50-5.5). A split entry into 1+1 at 125 with fees 1.25+1.25 has the same
cash/quantity as one fill of 2 with fee 2.5. Re-delivery of an already processed
FillID changes no quantities, fee totals, audit count or trade count.

**R7 — Pending quantities prevent duplication; reject/cancel is not a fill.** Before
the first Buy 2 is filled, repeat execution cannot stack another Buy 2. After a
partial fill of 1, only the remaining 1 is pending. A rejected entry leaves cash
1000/flat with no trade; a canceled partial entry retains its actual filled unit.
Example: Buy 2, fill 1 at 125, cancel the rest -> cash 875/position 1, not flat and
not 2. An exit based on that state sells exactly 1 at 150 -> cash 1025/PnL 25.

**R8 — Pausing blocks new decisions/execution, but not external fill accounting.**
Example: submit Buy 2, pause, then receive Fill 2 at 125 -> account must still
become cash 750/position 2. A close or execution call while paused throws and
cannot add an order. Resume restores admission, not prior filled economics.

**R9 — Durable attachment does not overwrite; committed fills and completed-day
restart preserve the same outcome.** Attaching a fresh engine to an existing
SQLite store must leave its cash 750 snapshot intact until explicit restore or
checkpoint. Entry fills persist both cash/position and their audit IDs. Example:
checkpoint after completed close(4), with cash 750/position 2 and exit pending;
recreate engine/indicators/venue, restore actual venue cursor plus persisted engine
state, then open(5) -> cash 1050, one closed PnL-50 trade and two distinct fills,
identical to an uninterrupted run. Unknown restored strategy IDs, duplicate strategy
or pending-plan IDs, next order ID 0, or a next ID overlapping tracked orders must
be rejected; no new exchange command is emitted during restore.

## Canonical replay rules

**R10 — Replay preserves USD-at-close versus units-at-open profile semantics.**
At close(3) plan USD 250 uses reference close 120 and contains no venue quantity.
At open(4) parity submits 2 units at 125; ordinary MOCK close-reference conversion
submits floor(250/120 on the 1e-8 grid)=2.08333333 units. They are intentionally
different configured profiles. Both must exit their actual held units. Parity's
double economic mirror has cash 750 after entry; the perpetual MOCK venue retains
settled collateral cash 1000, with unrealized PnL -50/equity 950 at close(4).
Do not assert that the two account cash models are interchangeable.
An additional control campaign uses the canonical default genesis USD 100000 and
entry weight .0025: the same USD 250 target buys 2 at 125, leaves parity cash 99750,
and exits at 150 for final cash 100050. A standalone manual control uses weight
.005 at close 100 to request 5 units; selling at 150 after buying at 125 realizes
125 and leaves canonical settled cash 100125. These controls do not replace the
explicit USD-1000 configuration requirements above.

**R11 — Opening execution uses opening evidence only.** `onExecutionOpen` admits
orders; explicit open-only MarketBarObservations perform canonical matching, as in
the canonical runner. Their OHLC are all the supplied open and contain no future
high/low/close. Parity ignores volume capacity and has zero fees/slippage. Example:
volume 0 at the open still fills the approved 2 units at 125; a changed future
close cannot change that fill. Closed slices subsequently mark and decide.

**R12 — Replay time progresses strictly; invalid replay delivery cannot double
trade.** Repeated/out-of-order closes and repeated/earlier opens throw without new
fills or positions. Example: deliver close(4) twice after buying 2 -> still one buy
fill, position 2 and the same pending exit. An execution open <= pending close(3)
is rejected, preserving its plan for the real open(4). A valid open without a pending
plan advances the phase without creating orders.

**R13 — Recovery snapshots are only valid at completed, quiescent parity boundaries.**
Snapshots require a completed close and no fill waiting at the open edge; normal
MOCK resume is explicitly unsupported. Example: close(3) before admission is safe
(its pending USD 250 plan is included); immediately after submit at open(4), before
the venue fills, is unsafe. Restore must reject a missing signal checkpoint or
a signal timestamp inconsistent with the saved completed close.

**R14 — Completed-day replay restart is economically identical.** Checkpoint the
actual MOCK journal and runtime/chaos resume state together; reconstruct new signal,
risk, planner, adapter and runtime objects with the same configuration. Example:
interrupt after close(3) or close(4), continue to close(5) -> the same two fills,
quantities 2, prices 125/150, parity cash 1050, canonical flat account, deterministic
identities and economic/stream fingerprints as the uninterrupted run. Resume
restores released history and signal persistence, not future market bars.

**R15 — Reconciliation and valid market evidence gate routing.** If the venue is
unavailable, new submissions are blocked and no fill/cash is invented. Missing
open prices reject before a command is sent, preserving the pending plan for retry.
`(assumed)` Validate a complete cross-section before letting any of its malformed
contents fill an admitted order. Example: an invalid close(4) slice containing BTC
twice must reject with the previously admitted Buy 2 still unfilled, rather than
applying one of those bars and then discovering the duplicate.

**R16 — Diagnostics are observational.** Enabling decision diagnostics changes
neither quantities, prices, cash, IDs nor fingerprints. Example: the base five-day
run with diagnostics on/off ends flat at 1050 with the same two fills and economic
and stream evidence. Observed close(3) still explains USD 250; analytics cannot
turn HOLD into a daily resize.

## Manual runtime rules

**R17 — Manual replay limits reject rather than clip and leave rejected intent
without side effects.** Use the strictest configured gross and per-asset limits
across strategies. Example: configs (gross .75, asset .5) and (gross 1, asset .3)
yield .75/.3. A BTC request .31 is rejected; BTC .3 is allowed. With gross .4 and
asset .3, BTC .25 plus ETH .25 exceeds gross and is rejected. Reject negative/NaN
weights, empty assets, missing requested/held prices, or timestamps <= latest
open/close. A rejection keeps the old pending strategy plan, cash and next order ID.

**R18 — A valid manual replay target supersedes pending strategy intent and
resynchronizes at the next close.** At close(3) USD 250 is pending; operator requests
BTC .5 at open(4)=125 -> discard the strategy plan, buy 4 units for 500, parity cash
500. In a continuing-signal variant close(4)=130, equity 500+4*130=1020; the next
close resynchronizes the HOLD strategy to approved target .25*1020=255, requiring
a sell intent instead of holding the manual 4 units. Cash-only target closes omitted
holdings exactly. A pending manual re-sync is not representable in ReplayResumeState
and therefore cannot be checkpointed. Manual operations do not fabricate scheduled
close/signal/planning evidence counts.

**R19 — The standalone manual pipeline enforces explicit long-only allocations.**
`ManualTrading` validates identity, sum(asset weights + cash)=1, mapped symbols and
configured limits before venue dispatch. Example with limits gross .75/asset .5:
BTC .6 rejected; BTC .4 + ETH .4 rejected; BTC .5 + cash .5 accepted. A cash-only
request while flat is Noop. This ordinary MOCK path sizes at the close: initial
equity 1000, BTC target .5, close 100 -> 5 units, even if execution open is 125.
Admission alone changes no holdings; matching 5 at 125 does. A subsequent cash-only
exit at 150 sells those exact 5, yielding perpetual realized PnL 125 and settled
cash 1125 with configured zero fees. Invalid sums, unmapped assets or missing
reference prices dispatch no command.

## Findings

### F1 — A newer flat signal retains an obsolete pending entry (R4)

`DecisionBehaviour.DISABLED_R4_NewFlatSignalRemovesAnUnexecutedOlderEntry` fails
because `hasPendingDecisions()` remains true after close(4), although the signal
is now zero and no units were ever bought. `DecisionEngine::onBarClose` skips empty
rebalance plans without removing the previously stored strategy intent. The old
close(3) entry survives under the newer batch timestamp. This disagrees with the
explicitly assumed R4 rule. The empty-intent expectations were retained.

Fixed (2026-10-19): A newer empty rebalance plan removes that strategy's obsolete pending intent; unavailable sizing still defers planning.

### F2 — TradingEngine admits same-close market execution (R5)

`TradingBehaviour.DISABLED_R5_SameCloseExecutionCannotCreateAClosingPriceEntry`
calls execution at close(3) with its close price 120. It returns without throwing
and leaves an active exchange order plus tracked local order. The specified timing
rule requires rejection before dispatch. The ordinary market path through
`TradingEngine::executePendingPlans` and `QuantityOrderPlanner` has no later-than-
decision guard (the research-stop path has one). The assumed generic-path timing
requirement and original expected exception/no-orders assertions remain unchanged.

### F3 — MOCK reconciliation assumes a different initial cash than its account

The public `MockAccountingConfig` accepts `initial_cash="1000.00"`, and the account
is created with that balance. `MockReconciliation::beginLedger` in
`lib/src/exchange/mock/reconciliation.cpp` instead initializes its expected ledger
unconditionally to `"100000.00"`. Before any trade, this creates false cash drift;
the routing gate then blocks legitimate replay/manual orders. Observed consequences
are zero entry fills, parity cash staying 1000 rather than reaching 750/1050, manual
exceptions stating `Manual rebalance blocked: reconciliation is not clean`, and
`ManualTrading` reporting `ReconciliationBlocked` instead of `Submitted`.
A targeted re-run of the disabled R10 case with `--gtest_also_run_disabled_tests`
confirmed the report verbatim: `ledger-derived settled cash does not match venue
snapshot: local=100000.00000000 venue=1000`, followed by zero rather than one entry
fill. This distinguishes the false drift from a missing market price or risk cap.

Eight original USD-1000 cases retain their full expectations and are disabled:

- `ReplayBehaviour.DISABLED_R10_CloseNotionalBecomesNextOpenQuantityAndFinalProfit`
- `ReplayBehaviour.DISABLED_R12_DuplicateAndOldClosesCannotReapplyFillsOrAdvanceThePlan`
- `ReplayBehaviour.DISABLED_R14_CompletedDayRestartMatchesUninterruptedEconomicAndStreamState`
- `ReplayBehaviour.DISABLED_R16_DiagnosticsLeaveCashFillsAndFingerprintsUnchanged`
- `ReplayBehaviour.DISABLED_R17_StrictestManualLimitsRejectInsteadOfClipping`
- `ReplayBehaviour.DISABLED_R18_ManualTargetDiscardsOldPlanAndResyncsHoldAtNextClose`
- `ReplayBehaviour.DISABLED_R18_CashOnlyManualTargetClosesOmittedHoldingsAndNeedsTheirPrices`
- `ManualBehaviour.DISABLED_R19_CloseSizedFiveUnitsFillAndCashOnlyExitRealizes125Dollars`

To isolate other runtime rules, six scenarios without absolute-cash expectations
were configured with canonical genesis 100000 and entry weight .0025. Their target
is still USD 250 and their expected entry still 2 units; no expected value changed.
Five then passed; the invalid-cross-section case exposed the independent F4 below.
The original custom-cash cases above remain as evidence, rather than changing their
expected money to match the blocking implementation. Added default-genesis controls
also pass a full replay/restart/diagnostics comparison at cash 100050 and a manual
five-unit round trip at cash 100125, computed from the worked examples in R10.

Fixed (2026-10-19): Reconciliation and replay evidence seed their expected ledger from configured initial account cash, retaining the default USD 100000 genesis.

### F4 — Replay fills orders before validating the complete market slice (R15)

`ReplayBehaviour.DISABLED_R15_InvalidCrossSectionCannotFillBeforeItsValidation`
uses the canonical-genesis control so the Buy 2 really is admitted. A close(4)
slice containing BTC twice throws `std::invalid_argument`, but afterwards the
position is 2, a parity fill exists, and tracked filled quantity is 2 instead of
the expected 0. `ReplayRuntime::onClosedSlice` sends bars to the venue matcher before
`RollingMarketState::append` rejects the duplicate. This violates the assumed
validate-before-side-effects requirement R15. All original assertions are retained.

Fixed (2026-10-19): Replay validates every venue bar and appends the complete validated cross-section before matching any orders.

### Step 3 validation and test setup

The first requested `ninja -C build -j2` reported no work because the runtime
Meson file was still a placeholder. The area target was registered, missing replay
and manual cases were completed, and both restart fixtures were checked against
real public collaborators. The fill cursor uses `FillID`, and the restored short's
terminal order carries its actual fill timestamp. No production file was changed.

The first registered build completed without warnings. Its 47 cases produced 31
passes and 16 failures. After isolating the canonical-genesis setup and adding the
two worked control campaigns, 49 cases produced 38 passes and 11 failures; all
11 disagreements are now retained with `DISABLED_` names and `// FINDING:` comments.
No existing expected value was changed, and no specification rule was relaxed.
The final `ninja -C build -j2` completed without warnings;
`meson test -C build --no-rebuild --print-errorlogs unit_runtime` passed with
38 active cases, 11 disabled cases, no skips and no remaining active failures
(49 total). Final output is in `build/meson-logs/testlog.txt`. Changes are confined
to this spec and `tests/unit/runtime/`; no commit was made.
