# Planning behaviour specification

## Purpose and boundaries

Planning turns approved risk/rebalance intent plus filled and pending execution state
into orders that move each strategy toward its target. A plan is intent, never a fill:
it must not change cash, filled positions or the order manager.

Sources: `docs/codex/ARCHITECTURE_FULL.md` (Timing and economics, Durability),
`lib/README.md` (Planning versus execution, Replay timing), and the public headers
`execution/planning/{notional_order_planner,quantity_order_planner,target_resolution,
quantity_order_builder,planning_state}.h`, `contracts/{decision_batch,
notional_order_planning,order_planning}.h`, `rebalance/rebalance_plan.h`,
`execution/{execution_order,order_manager}.h` and `market/price_snapshot.h`.

Two related APIs have distinct contracts. NotionalOrderPlanner consumes explicit
`target_notional_usd` at close(T). QuantityOrderPlanner resolves the in-process
weight × frozen reference-capital intent at execution prices, normally open(T+1).
It is not a converter for PlannedNotionalOrder. The runtime/adapter connects the
notional path to the venue; that conversion and its fills belong to other areas.

Venue lot steps, minimum quantity and minimum notional are enforced at the venue
boundary, not by these exchange-agnostic planners (which accept no venue rules).
For example, an approved USD 1 target at price 3 remains USD 1 / quantity 1/3 here;
a venue with step 0.1 and minimum notional USD 10 must reject/normalize downstream.
Tests must not silently introduce a fictitious default grid or minimum here.

All examples use synthetic BTC/ETH, USD, strategies 7/9, close date T=20260105,
and execution date T+1=20260106. Timestamp is a YYYYMMDD key, not Unix time.
Expected arithmetic below is computed independently; revision/hash bytes and
identity string formatting are deliberately not prescribed.

## Rules with worked examples

### R1 — Freeze approved monetary targets at the decision close

The notional planner uses explicit USD targets, without recomputing risk allocation
from weight, capital, execution opens or later market data. Its audit fields retain
close(T), decision timestamp and supplied state revision. Example: explicit USD 250,
weight 0.9 and reference capital USD 1,000 still mean USD 250. With no filled/pending
position, close 100 or 125 produces a BUY of USD 250. Quantity is deferred.

### R2 — Compute signed deltas from filled plus remaining pending exposure

At close(T), current USD = filled quantity × close; pending USD = signed remaining
open-order quantity × close; delta = target − current − pending. Positive delta buys,
negative delta sells; order notional is its absolute value. Examples at close 100:
filled +2 to target 500 buys 300; filled +5 to 200 sells 300; filled −2 to −500
sells 300; filled +2 to −300 sells 500. No account-level netting may erase the
strategy attribution of those orders.

### R3 — FLAT closes actual held units, including shorts and price gaps

FLAT means target zero, independently of old reference capital or entry price.
Example: held +3, close 100 gives a USD 300 SELL intent; at execution open 250,
the quantity planner sells exactly 3, not 300/250=1.2. Held −3 requires BUY 3.
Repeated FLAT on an already flat strategy creates no order.

### R4 — HOLD preserves filled quantity; an already-covered target does nothing

Missing strategy/asset intent means HOLD. The quantity resolver also accepts explicit
HOLD. Example: held +3 at open 125 means USD 375 and quantity 3, with no order;
a separate ETH decision must not resize BTC. Held −3 behaves symmetrically. A
USD 200 target with held +2 at close 100 needs no order or ID allocation. The
quantity equivalent held +2, target USD 250, open 125 also needs no order.
Notional LIVE DTOs omit HOLD rather than accepting explicit HOLD entries (R9).

### R5 — Pending quantities prevent duplicate orders and use only the unfilled remainder

Example: order BUY 5, already filled 2, filled position +2, remainder +3. Target +5
(or USD 500 at close 100) is covered: no cancel, no new order, no ID consumption.
Likewise held −2 plus SELL remainder 3 covers target −5. A fresh planner using
restored orders/positions must make the same economic decision. Terminal orders
contribute zero pending quantity.

### R6 — Cancel conflicting work before replacement, and await acknowledgement

Example: filled +2, pending BUY 3, new target +1. Request cancellation of the old
order only; do not simultaneously submit SELL 1. While cancel is requested but
unconfirmed, remainder is still pending and there is no repeat cancel/new order.
After a terminal cancel acknowledgement, target +1 requires SELL 1 (USD 100 at
close 100). Even a pending BUY 2 toward a target +5 from zero must be canceled
before replacement if it does not cover the current target. An orphan pending
BUY with no filled position and missing asset intent must be canceled to HOLD zero.

### R7 — Replanning and restart produce stable identities and allocation

Identical immutable inputs and next ID reproduce orders/identities and next ID.
Example: next ID 40, BTC and ETH both require orders; BTC gets 40 and ETH 41,
next ID 42, independent of unordered asset insertion. Cancellations and no-ops
consume no IDs. Changing the execution-state revision changes the notional economic
identity, preventing reuse of an identity for different state.

### R8 — Planning has no execution side effects

Example: held +2 with a USD 500 target produces a USD 300 intent, but held remains
+2 and no order appears in OrderManager. Calling the planner twice without applying
the output repeats the same plan, rather than treating the first plan as a fill.
Snapshot copies also remain unchanged when source positions/orders later change.
The caller persists/applies plans; planner calls alone cannot provide durable deduplication.

### R9 — Reject malformed notional intent rather than trade on ambiguous inputs

Reject zero next ID/revision/decision date, mismatched batch/intent dates, duplicate
configured strategy IDs, duplicate intents, unknown intent/position strategy IDs,
missing USD targets, orphan USD targets, nonfinite USD targets, explicit LIVE HOLD,
and nonzero FLAT USD targets. Missing close for a held, targeted or pending asset
is an error. Examples: FLAT with USD 10 must not submit a BUY 10; a USD 250 BTC
target without a BTC close must not receive a fabricated price. Public PriceSnapshot
rejects 0, negative, NaN and infinite prices before either planner can use them.
No partial result is returned on rejection.

### R10 — Resolve quantity targets at execution price with frozen capital

For the in-process path, desired USD = approved weight × decision reference capital;
quantity = desired USD / supplied execution price. Example: weight 0.25 and frozen
capital 1,000 give USD 250, hence BUY 2 at open 125 or BUY 5 at open 50. Negative
weight −0.25 gives SELL 2 at 125. Held +2 to target −2 needs SELL 4. Strategy targets
remain separate even if their account exposure nets to zero: +250 and −250 at
125 require BUY 2 for one strategy and SELL 2 for the other.

### R11 — Decisions precede execution and use consistent strategy identity

The decision describes completed close(T); executable quantity is for a later open.
No future close/high/low is supplied to either planner. Market quantity planning must
reject execution at or before decision time rather than permit same-close/future
intent execution (assumed enforcement at this API boundary). Example: decision
20260105 executes at 20260106; execution 20260105 or 20260104 is rejected. Reject
unknown/duplicate configured or intent strategy IDs, unknown position strategy IDs,
inconsistent intent/batch dates and zero next ID. Empty decision batches create no
new quantity commands and preserve the next ID.

### R12 — Target resolution retains signs and refuses missing prices

StrategyTargetResolver starts from filled quantities valued at supplied execution
prices, then overrides only explicit FLAT/weight decisions. Example: held BTC +3
and ETH −2, prices 125/50, ETH FLAT gives BTC USD 375 and ETH 0. A −0.25 target
with capital 1,000 is USD −250, resolving to −2 at 125. Missing held/target prices
must throw, never silently become zero, even when the desired decision is an exit.
The compatibility RebalancePlan overload must preserve the same arithmetic.

### R13 — Quantity builder applies per-strategy deltas safely

Example: held +2, target +5, no pending means BUY 3; held +2, target −3 means
SELL 5. Orders carry caller-supplied created/activation dates. Current and target
vectors must align in size; mismatches throw before returning any plan. Assets
present only in current positions still require flattening when absent from the
explicit quantity target. IDs and cancels follow R6/R7.

### R14 — Final guard forbids duplication, wrong direction and overshoot

Effective position = filled + pending. Example: filled 2, pending 1, target 5 requires
+2: BUY 2 and BUY 1 are allowed; BUY 3, SELL 1, and any order for an already
covered target 3 are forbidden. Short example: filled −2, pending −1, target −5
allows SELL 2 only up to the remaining 2. Nonfinite state inputs are rejected.
Explicit nonnegative epsilon defines the no-op tolerance: a required delta 0.0005
with epsilon 0.001 creates no order; negative/nonfinite epsilon is invalid.
Smaller orders remain valid so an adapter can honor venue maximum sizes.

### R15 — Snapshot revision follows economically relevant state across restart

Revision is nonzero and depends on configured strategies, filled positions,
tracked directional-order identity/strategy/asset/side/quantity/status/fill progress/
cancel flag, and next ID. Container insertion order, order-vector order and strategy
list order do not change it. Example: strategies 7/9 with BTC +2 and ETH −3,
pending BUY 5 filled 2, next ID 40 have the same revision after reconstruction;
changing held +2 to +3, remaining 3 to 2, cancel flag, status, side, asset, strategy,
order ID or next ID changes it. Human messages/exchange labels/update time are not
planning economics. Snapshot is a value copy; mutating sources cannot rewrite it.
Resting LIVE position-stop protection is excluded from the planning snapshot and
must not invalidate an otherwise identical plan when execution replaces protection.

### R16 — Conditional research entries retain frozen trigger economics

Stop entries open only from flat, with a positive finite trigger and nonzero target.
Example: capital 1,000 × weight 0.25, long trigger 100, next open 90: submit BUY
2.5 with trigger 100, created at T, active from T+1. Short weight −0.25, trigger
100, next open 80: submit SELL 3.125 (=250/80); at open 120 submit SELL 2.5
(=250/100). These are commands, not assertions that a trigger/fill occurred. Reject
stops on an already-held position and activation at/before the decision close.

### R17 — Research protective covers do not represent directional pending exposure

Example: filled short −2 plus a resting bracket BUY cover 2. HOLD keeps −2 and
leaves protection alone; its conditional BUY must not provoke an extra SELL 2.
A timed FLAT may request cancellation of the cover and a BUY 2 close command;
execution/simulator owns cancel-before-fill ordering. No fill or protection lifecycle
is performed by planning. This research bracket is distinct from the LIVE
position-stop snapshot exclusion in R15.

## Invariants and validation boundary

- No planner mutates filled positions, cash or tracked order state.
- Filled plus remaining pending exposure covers a target at most once per strategy/asset.
- Signed quantities and BUY/SELL sides agree, including short covering and sign flips.
- Same inputs after reconstruction give the same plan; only actual execution changes state.
- Price snapshots have no embedded timestamps. The caller must select the correct
  close/open snapshot; tests supply explicit synthetic references and dates. The
  standalone planner cannot prove that an arbitrary price was visible at T.
- Venue rounding/admission, order submission, fill-ID deduplication, cash/PnL and
  canonical runtime close-to-open conversion are exercised by their owning areas.

## Findings

### F1 — Market quantity planning does not reject execution at/before the decision close (R11)

`QuantityPlanner.DISABLED_R11_RejectsExecutionAtOrBeforeDecisionClose` retains
the expected `std::invalid_argument` for both invalid execution dates. With decision
date 20260105, capital USD 1,000, BTC weight 0.25 and reference price 100, calling
`QuantityOrderPlanner::createPlan` with execution date 20260105 or 20260104 throws
nothing. The first run reported both failed exception assertions; all other 58 tests
passed. This disagrees with R11's explicitly assumed enforcement at this API boundary.

Source inspection of `lib/src/execution/planning/quantity_order_planner.cpp` confirms
that market orders receive the supplied execution date without comparing it to the
decision date; the ordering check is conditional on a nonzero entry-stop price.
The passing R16 rejection test exercises that stop-specific check. This finding is
an API validation gap against the assumed rule, not evidence that an upstream
runtime actually executes future decisions. The spec and expected exception remain
unchanged; production code is untouched.

Fixed (2026-10-19): Reject nonempty quantity decision batches when execution is at or before the decision close.

Validation (WSL, 2026-10-19): `ninja -C build -j2` completed without compiler
warnings. After disabling F1,
`ALGOTRADING_SOURCE_ROOT="$PWD" build/tests/unit/planning/test_unit_planning --gtest_brief=1`
exited 0: 59 tests total, 58 passed, 1 disabled, 0 active failures. Running directly
with Meson's quiet-logging, allocator-perturbation and library-path settings also
exited 0 with the same counts.

Runner limitation: `meson test -C build --no-rebuild --suite unit unit_planning --print-errorlogs`
reported a 30-second timeout despite capturing all 58 passing tests and reporting
the child result as `exit status 0` in `build/meson-logs/testlog.txt`. The cause of
the Meson runner timeout is unconfirmed; it is recorded separately from F1 and
does not justify disabling additional behaviour tests or changing expectations.
