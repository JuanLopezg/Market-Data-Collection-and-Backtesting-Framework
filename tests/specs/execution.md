# Execution behaviour specification

## Purpose and sources

Execution owns executable order state and applies actual venue fills to account,
strategy quantities and trade records. Planning outputs and exchange acknowledgements
are not money movements. Sources, in priority order: `docs/codex/CONTEXT_FULL.txt`,
`docs/codex/ARCHITECTURE_FULL.md` (Durability, Timing and economics), `lib/README.md`
(Planning versus execution), and public headers `execution/{execution_engine,
execution_order,order_manager,fill}.h`, `exchange/exchange.h`,
`contracts/order_planning.h`, `testing/fake_exchange.h`, `account/account.h`, and
`analytics/{trade_recorder,trade_record}.h`.

Tests use real ExecutionEngine, OrderManager, Account and TradeRecorder, with the
library's deterministic asynchronous FakeExchange standing in for the external
venue. A test-local exchange may delegate to FakeExchange to advertise conditional
capabilities, record outbound command ordering or inject a dispatch failure.
No sockets, databases, wall clock, sleeps or historical storage are needed.

Examples start with USD 1,000 and zero positions; strategies 7/9 are Alpha/Beta.
Dates are YYYYMMDD: close(T)=20260105, open(T+1)=20260106, later=20260107.
The fake charges 1% where explicitly used; direct Fill payloads carry their own
commission in USD. Numbers below are calculated by hand, not sampled from code.

## Behaviour rules and worked examples

### R1 — Commands and acknowledgements never imply fills

Submitting BUY 5 does not spend USD 500 or create +5 BTC. Before fills, cash stays
1,000 and account/strategy positions stay zero through Created, Submitted and
Accepted. A status-only Filled update also cannot invent money or filled quantity;
only Fill events carry executed units and costs. No trade record exists without a fill.

### R2 — Lifecycle tracks the remaining signed quantity

Created -> Submitted -> Accepted -> PartiallyFilled -> Filled is the normal path;
Submitted may go directly to a terminal venue response. BUY 5 partially filled 2
has remaining/pending +3; SELL 5 partially filled 2 has pending −3. Rejected,
Canceled and Filled orders contribute zero pending exposure and are not open.
Lifecycle quantity comes from actual fills, not status names. Transitions that
contradict an existing terminal status throw rather than reopen the order.

### R3 — Cancellation retains exposure until acknowledgement and admits late fills

BUY 5 filled 2 then cancel requested still has pending +3. Cancel confirmation
sets pending to zero without undoing the +2 fills. A late additional BUY 1 after
cancel is real: total filled becomes 3, remaining unexecuted request 2, status
stays Canceled and pending stays zero. A late fill completing all 5 may mark Filled.
With the BUY 2 at 100 fee 2 starting fill, the late BUY 1 at 110 fee 1.10
leaves cash 798−110−1.10 = USD 686.90 and position +3.
Rejecting an already-filled order is a contradiction. Canceling a terminal order
or an unknown ID is an error. Repeated execution cycles must not repeat cancel
commands while confirmation is outstanding.

### R4 — Stale status echoes cannot regress lifecycle or business time

After partial BUY 2 of 5, a delayed Accepted echo is ignored even if delivered
later: fill quantity 2, status PartiallyFilled and update timestamp remain unchanged.
After Filled, delayed Submitted/Accepted/PartiallyFilled echoes cannot reopen it.
An identical terminal update does not apply another fill. For the same lifecycle
stage, an older timestamp must be ignored rather than move updated_at backwards
(assumed timestamp-ordering rule). Example: Accepted at 20260106 followed by
Accepted at 20260105 retains 20260106 and its exchange identity.

### R5 — Reject invalid economic payloads

Orders require a nonempty asset and positive finite quantity; tracking requires a
nonzero unique order ID. Fills require a nonempty asset, positive finite quantity
and price, and finite nonnegative commission. Examples: quantity 0/−1, price 0,
commission −1, NaN or infinity must not create positions or cash changes. A zero
fee is valid. A malformed default-constructed order must also be rejected at the
engine submission boundary (assumed defensive validation of public mutable DTOs).

### R6 — A fill must match a known order and never overfill it

Fill strategy/asset/side must match the tracked request. An unknown order or
rejected order cannot receive a fill. For BUY 5 already filled 2, another BUY 4
must throw: filled remains 2, pending remains 3, cash/positions are unchanged.
A fill for strategy 9 cannot debit strategy 7's order, nor may an ETH/SELL fill
satisfy a BTC/BUY order. Exact remaining quantity 3 is accepted.

### R7 — Fill-ID idempotency survives restart; invalid attempts do not consume IDs

A positive FillID is the economic identity. Deliver BUY 2 at 100, fee 2, ID 101
twice: order filled quantity 2, cash 798, positions +2 and one analytics fill.
Restoring the processed ID set retains this protection. Rejecting a mismatched,
unknown-order, rejected-order or overlarge fill must not add its ID to the accepted
set (assumed atomic rejection guarantee). Example: reject ID 101 with wrong asset,
then deliver corrected BTC ID 101; it must execute once, not disappear as a duplicate.
These tests use positive IDs for economic deduplication. The public Fill DTO also
permits a default zero ID; this specification does not define retry guarantees for
fills without a positive economic identity.

### R8 — Every accepted fill updates all economic projections once

BUY 2 at 100 fee 2 gives cash 798, account +2, Alpha +2; marked at 100 equity is
998 and open-trade PnL is −2. A later BUY 3 at 110 fee 3.30 completes BUY 5:
cash 464.70, positions +5 and total fees 5.30. SELL 2 at 100 fee 2 from flat gives
cash 1,198, position −2 and equity 998. Filling SELL 4 at 110 fee 4.40 after the
BUY 2 example flips to −2 and cash 1,233.60. Closing the initial long realizes
(110−100)×2−2−2.20 = USD 15.80; the other 2.20 commission belongs to the new short.
Account/strategy/trade recorder changes must be visible together in the fill
persistence callback, with the original FillID and fee. No callback carrying a new
fill occurs on duplicate delivery.

### R9 — Preserve per-strategy attribution when account positions net

Alpha BUY 2 at 100 fee 2 plus Beta SELL 2 at 100 fee 2 yields account BTC 0,
Alpha +2, Beta −2, cash 996 and two open campaigns. Neither strategy may inherit
or erase the other's units. Aggregate account units equal the sum of filled
strategy units when initialized consistently.

### R10 — Apply the ordered exchange event stream in order

A queued Accepted, Fill BUY 2, partial update, Fill BUY 3, Filled update must
leave a filled BUY 5, cash 464.70 and matching account/strategy units, preserving
both fill IDs and prices. Processing a duplicate Fill or stale status in the same
stream cannot reapply economics. The external single-event API returns only after
persistence succeeds; stale no-op status echoes may return false without persistence.

### R11 — Persist the complete plan before sending any command

For an existing BUY 5 to cancel and two new orders (IDs 2/3, next ID 4), the first
persistence callback sees the cancel-request flag plus both new Created orders and
next ID 4, before any new submit/cancel reaches the exchange. Only then may commands
leave. Submitted state is persisted after dispatch. Cash and positions remain
unchanged. This is an order-of-durability requirement, not a prescribed count of
internal calls beyond the meaningful committed snapshots.

### R12 — Validate the whole plan before changing owned state

Reject zero execution date, zero/regressing next ID, duplicate/reused/out-of-range
submit IDs, unknown strategy, duplicate cancels, unknown/terminal/already-requested
cancels, malformed quantity and nonfinite target exposure. Example: a valid BUY 2
followed by an unknown-strategy BUY 3 must submit neither and preserve cash,
positions, all order state, next ID and cycle markers. Nonfinite target validation
must similarly precede marker mutation (assumed complete rejection guarantee).

### R13 — Failed persistence or dispatch leaves recoverable intents, never imaginary fills

If the first plan persistence callback throws, no command may reach the exchange.
Staged Created orders are not Submitted and no cash/position changes happen.
If dispatch of order 2 fails after order 1 was sent, the last committed complete
snapshot still includes both requests: order 1's submitted progress and order 2's
Created intent can be reconciled on restart. A fill persistence callback failure
propagates so transport does not ACK. Reconstructing from the last committed snapshot
and redelivering the fill must apply it once; no same-object rollback is assumed.
Recovery/outbox dispatch itself belongs to the recovery area.

### R14 — Resolve close decisions at the supplied later execution open

Weight 0.5 × frozen close capital 1,000 is USD 500. At next open 125 submit BUY 4;
submission/acceptance do not change positions. A partial fill 1 leaves pending 3,
and at a 1% fee the cash is 1,000−125−1.25 = USD 873.75.
Repeating the same target must not submit another BUY 3/4. New target USD 125
(quantity 1) cancels pending work first; only after cancel confirmation may a later
target requiring more units be submitted. FLAT of filled +1 sells exactly 1 at any
execution price. Input selection/no-lookahead belongs upstream; the planning area's
R11 covers its assumed market date guard, rather than duplicating that test here.

### R15 — Manual orders leave daily-cycle state alone

After a daily no-op cycle at 20260106 targeting zero, manually submit BUY 2 on
20260107. The order must be durable before dispatch, but last daily execution date
remains 20260106 and daily target remains zero. A manual order must not suppress the
next daily decision. Invalid manual timestamps/IDs/unknown strategy are rejected as
in R12, and manual submission alone cannot change money.

### R16 — Restore authoritative quantities and deduplication without exchange effects

Restore cash 798, account/Alpha BTC +2, BUY 5 partially filled 2, processed ID 101,
next ID 11, last cycle 20260106. Restoration sends no commands, preserves pending
3, and replay of 101 changes nothing; a new fill ID 102 for 3 at 110 fee 3.30
produces cash 464.70 and +5. Missing configured strategy state restores flat.
Next ID must be nonzero and above every tracked ID; unknown strategy positions,
duplicate orders/fill IDs, zero processed IDs and invalid filled progress reject.
OrderManager restoration must leave its prior valid state intact on invalid input.
TradeRecorder analytics are rebuildable by the runtime from the persisted fill log;
ExecutionEngine restore does not fabricate historical campaigns.

### R17 — Unsupported conditional commands fail before persistence or dispatch

An ordinary exchange with no stop/bracket capability cannot accept a research
stop BUY 2 at trigger 100 or a short bracket parent/child; no ID, marker or pending
order may be staged. Stop entry must activate after its decision close, with finite
positive trigger; a bracket cover is BUY, has positive protective price and parent
link, and cannot itself be an entry. Example: created 20260105, active 20260105
is invalid; active 20260106 is valid for a capable exchange.

### R18 — Research bracket covers are based on actual parent fills and created once

A capable venue receives short entry SELL 5 with protective price 120. Parent fills
only 2 before cancellation: terminal confirmation creates one BUY cover of exactly
2, linked to the parent and using protective price 120. Unfilled/rejected parents
create no cover. Repeated terminal events, including after the child terminates or
a restart restores both orders, must not create another cover or consume another
ID. Creating/submitting a cover is not itself a fill and does not flatten the short.
For a fully filled SELL 2 at 100 and BUY cover 2 at 120 with 1% fees,
cash = 1,000+200−2−240−2.40 = USD 955.60; the closed short PnL is −44.40.

### R19 — Resting position-stop changes preserve the daily cycle and protection ordering

LIVE position stops are SELL orders with a positive position_stop_price, independent
of research brackets. Replacing old stop ID 1 (2 units at 90) with new ID 2 (2 units
at 95), next ID 3, first persists both intents, then sends the new protection before
canceling the old. Cash/position and daily markers remain unchanged. These conditional
stops do not contribute directional pending exposure. Only open, not-yet-requested
position stops may be canceled via this API; ordinary trades/duplicate cancel IDs
are rejected before side effects.

## Invariants and test boundaries

- Filled order quantity never exceeds requested quantity beyond the documented
  floating-point tolerance (tests use exact integer/fractional examples).
- Pending = signed unfilled directional quantity, never a conditional protective cover.
- Account and strategy units change solely through accepted nonduplicate fills.
- Cash change per fill = −signed quantity × price − commission.
- Business dates are explicit; no future bars, prices, sleeps or wall-clock reads.
- Restore sends no commands; acceptance of a plan is separate from durable transport
  message deduplication and recovery dispatch.
- Money assertions at this boundary establish propagation to real collaborators;
  exhaustive cost basis, PnL and Backtester behaviour belongs to accounting.

## Findings

Steps 1–2 only: tests have not been built or run. No validated findings yet.
