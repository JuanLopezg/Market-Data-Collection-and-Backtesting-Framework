# Exchange adapter behaviour specification

## Purpose, sources and boundaries

Adapters translate executable commands and external evidence without inventing
fills, changing economic units or silently choosing another venue. This area covers
MessageExchange, BackendGateway, SimulatedExchange, and the canonical adapter
interface exercised through the real MockExchange facade.

Sources: `docs/codex/{CONTEXT_FULL.txt,ARCHITECTURE_FULL.md}`, `lib/README.md`,
`live_trading/README.md`, `live_trading/exchange_gateway/README.md`, and public
headers `exchange/{exchange,canonical_venue_adapter,exchange_gateway_adapter}.h`,
`exchange/adapters/*.h`, `transport/{message_bus,jetstream_bus,message_json,
message_subjects}.h`, `contracts/{execution_messages,exchange_snapshot_event,
exchange_snapshot_request,venue_orders,venue_account,venue_capabilities}.h` and
`testing/fake_exchange.h`. MOCK facade examples also use its documented canonical
admission/matching/account contracts; live HTTP protocols are outside this scope.

Two account models must stay distinct: the in-process simulator emits cash-style
fills for lib Account; canonical MOCK is a synthetic perpetual account whose cash
moves only for realized PnL/costs. Adapters must preserve the relevant contract.

Synthetic daily dates are D1=20260105, D2=20260106, D3=20260107. Standard execution
order: ID 40, strategy 7, BTC, BUY 2, created D1, active D2. All expected values
below are hand-calculated or invariants. Canonical MOCK control-plane fixtures
separately use supplied event keys 10/20/30, as its venue specifications do;
those keys are not substituted for daily timestamps in the execution DTOs.
Unit tests use a recording in-memory
MessageBus and real library collaborators. BackendGateway owns a concrete
JetStreamBus and cannot accept that test bus: its separate service executable
uses test_support::DockerContainer, skips when Docker is absent, and is registered
with suite ['service'] and timeout 300. No tests run in steps 1–2. No Internet,
real exchanges, external credentials, storage dataset or sleep-based business time.

## Rules and worked examples

### R1 — MessageExchange preserves executable economic intent on the public route

Submitting BUY 2 or SELL 2 publishes one SubmitOrderCommand to SUBMIT_ORDER with
ID 40, strategy 7, asset BTC, the same side/quantity, D1/D2 dates and optional
reference price 125. A manually priced order retains fill_at_reference_price=true
and price 125; publishing cannot itself spend 250 or create position 2. A host may
later apply an actual BUY 2 at 100 fee 2 and then cash becomes 798 from 1,000.
MessageExchange itself consumes no inbound events and never fabricates acceptance.

### R2 — Command identity is stable across retry/reconstruction and separate by order

Message metadata and durable publish ID refer to the same logical command.
Reconstructing the adapter and resubmitting order 40 produces the same message ID,
correlation and payload. Order 41 gets a different command identity. Cancel order
40 uses a stable cancel identity distinct from its submit identity and links the
same order correlation; it cannot cancel 41. Example: two submit attempts for BUY
2 remain one logical ID/2-unit request, never a new 4-unit instruction. Broker
retention/deduplication is the durable bus responsibility, not an in-memory cache
inside MessageExchange. Cancel has no business-time parameter, so it must not
invent a current wall-clock decision date.

### R3 — Publish errors propagate; command transport does not imply fills or ACKs

A failed publication of BUY 2 throws to the execution owner. A later retry retains
the exact logical identity. No accepted order/fill appears in drainEvents, and the
adapter must not subscribe to or steal inbound venue events. For example a failed
BUY at 100 leaves caller cash 1,000 and units zero until an actual fill is applied.
Persistence-before-dispatch and deduplication of FillIDs belong to ExecutionEngine.

### R4 — Invalid or unsupported wire intent rejects before publishing

Zero submit/cancel IDs reject. Empty asset, zero/negative/nonfinite quantity must
not publish a command that the receiving decoder cannot execute (assumed defensive
validation of mutable ExecutionOrder DTOs). Service schema rejects research entry
stops, protective short brackets and parent links instead of dropping their fields.
A valid LIVE SELL position stop with quantity 2 and position_stop_price 90 retains
that price through JSON; it is distinct from unsupported research brackets. Invalid
mixed conditions cannot be serialized as a plain market order.

### R5 — Simulator market fills use explicit eligible opens, once

Created D1/active D2 BUY 2 cannot fill at D1 or submission. At D2 open 125 and close
900 it fills exactly 2 at 125, timestamp D2, followed by Filled update. Changing
future close/high/low cannot move that open fill. If the asset is absent at D2, the
market order remains pending and may fill at D3's valid open. A later processOpen
or repeated drain cannot fill a terminal order again. Simultaneous orders receive
FillIDs in ascending local OrderID order, independent of insertion/map order.

### R6 — Simulator lifecycle/validation errors do not invent or lose economic work

Submission emits Accepted only. Canceling active ID 40 emits Canceled and prevents
its later fill; canceling an unknown/terminal ID does not invent a Fill. Duplicate
active IDs reject without replacing BUY 2 with SELL 5. Invalid open price 0/NaN
throws before removing that pending order; a later valid open 125 can still fill 2.
Manual fillAtPrice affects only the selected market order and uses its explicit
price (BUY 2 at 120), not another order's reference. Conditional orders cannot use
that market-only shortcut. Invalid commission and split fractions reject.

### R7 — Fees, split fills and injected rejects preserve quantity and cash

At 1% commission, BUY 2 at 125 has fee 2.50; lib Account cash becomes 747.50 and
marked equity 997.50 from initial 1,000. SELL 2 at 125 with the same positive fee
leaves cash 1,247.50 and marked equity 997.50.
Splitting BUY 4 at 100 at fraction 0.25 yields quantities 1 and 3 with distinct IDs, fees 1
and 3, total quantity 4/fee 4, cash 596 and equity 996. That split affects the next
fill only. One-shot reject emits Rejected with no fill and does not reject the next
valid independent order. Real FakeExchange and the simulator should agree on the
same explicit market fill economics; no venue fees are hidden in requested units.

### R8 — Simulator restoration preserves pending orders and FillID continuation

Restoring pending IDs 40/41 with next FillID 101 emits no unsolicited Accepted/Fill
history. Processing D2 produces those two executions with IDs 101/102, then next
ID 103. Restoring does not itself alter an account. Zero next ID, inconsistent map
key/order ID or invalid quantities reject before replacing prior valid state.
Durable service outbox handles replay of old events, not simulator restoreState.

### R9 — Research stop entries use a completed following bar, with explicit gaps

Long BUY 5 trigger 100 is not matched at the D2 open even if high is already present
in the supplied bar. At completed D2, high >=100 triggers at max(open,100). Open
95/high105 gives 5 at 100; gap open125 gives 4 at125, preserving 5×100=USD500;
unused entry cap cannot fill again and its terminal status is Canceled. Short
entries arrive already sized by planning: SELL 6.25, trigger100, open80/low70
fills 6.25 at80 (=USD500), not a second rescaled quantity. No trigger or missing
asset expires the one-following-bar entry. No quantity/fill may be created at D1.

### R10 — Research protective covers preserve filled units and OHLC assumptions

A BUY cover of 2, parent-linked and protective price120, stays separate from market
orders. On the entry bar D2, bullish open100/low90/high125/close110 permits the
entry-then-cover ordering and fills cover2 at120. A bearish entry bar open100,
close90 skips that cover even with high125 (documented synthetic OHLC ordering).
At later D3 gap open130/high140, fill at max(120,130)=130 for exactly2. Canceling
the cover before a timed market exit prevents both exits filling. Missing cover
asset evidence leaves it pending, unlike expiring research entry orders.

### R11 — LIVE position stops reduce actual longs at completed bars

SELL position stop2 at90 does not execute at processOpen; at completed bar open100,
low80 it fills2 at90. Gap open80/low70 fills at80. With requested quantity5 but held
long2, only2 can sell (fee1.60 at80/1%). Flat or short holdings cannot be increased
into a short by protection; trigger cancels the unusable stop without a fill.
Overlapping stops reserve units before accounting drains: held3, higher stop95
(ID41) requested2 and lower stop90 (ID40) requested2 fill2 then1, total3, never4.
Missing asset or untouched low does not expire a resting stop across days.

### R12 — Canonical context, capability and admission evidence remain explicit

Through CanonicalVenueAdapter&, real MockExchange identifies MOCK/Mock and advertises
its supported command/snapshot/fill/status capabilities. It does not advertise
unsupported DeadMansSwitch as a fallback. Correct BTCUSDT perpetual/USD catalog
identity and BUY0.2 at100 are admitted; a mismatched native instrument rejects with
no fill. Submitting is not execution; matching needs a subsequent explicit eligible
bar. Capability checks do not establish real private venue routing readiness.

### R13 — Canonical event order and read surfaces retain venue truth

The public facade emits operation result before Accepted. A subsequent 0.1 fill
at100 precedes its fee event (-0.004 USD) and resulting partial status, all through
one ordered Event handler. Account snapshot then reports cash99,999.996 and +0.1;
open-order view reports requested0.2/cumulative0.1; these reads do not mutate venue
state or the economic fingerprint. Known order query reports actual lifecycle;
unknown ID99 returns UnknownRequiresReconciliation, never a fabricated fill.
Fill-backfill bounds are inclusive: executions at20/30 queried over[20,20] return
only the first, preserving native FillID/side/quantity/price.

### R14 — Canonical retries/restart/ambiguous receipts cannot duplicate orders

Same request/payload after fresh reconciliation returns a result with no second
Accepted order. Cancel and modify preserve identity and emit actual lifecycle
updates. A lost submit receipt can leave one accepted0.2 order while omitting the
operation receipt; callers must reconcile before retry, not infer non-submission.
Delayed receipt is released by explicit poll without any fill/second acceptance.
Reopening the same durable MOCK directory restores cash/units/fingerprints without
re-emitting historical fills when a handler is attached; fresh snapshot/backfill
must expose the recovered truth. No silent switch to another venue is permitted.

### R15 — BackendGateway preserves commands on private routes

Submit BUY2 ID40, cancel40 and snapshot request travel to BACKEND_SUBMIT_ORDER,
BACKEND_CANCEL_ORDER and BACKEND_EXCHANGE_SNAPSHOT_REQUEST, retaining producer
message IDs, correlation IDs, dates and economic payload exactly. They must not
loop onto public execution subjects. Backend command publication itself emits no
OrderUpdate/Fill/snapshot callback. JetStream, not the adapter, owns durable dedup.

### R16 — Backend status/fill delivery preserves stream order and ACK follows handlers

Accepted40, Fill101 BUY2 at100 fee2, Filled40 arrive through one ordered consumer
in that order; Fill reaches the caller unchanged. A successful callback permits ACK.
A callback throwing (for example failed persistence before applying Fill101) requests
retry rather than losing that fill. After successful retry a host Account ends at
cash798/position2, not cash596/position4. The test callback fails before applying
the first attempt; operational exactly-once enforcement is the host's responsibility.

### R17 — Backend missing handlers and future frontier defer rather than drop evidence

Until all required handlers are installed, inbound Fill101 is Retry, not ACK. If
the permitted business frontier is D2, a message produced at D3 is withheld; after
raising frontier to D3 it is delivered with its original timestamp/ID. The adapter
uses metadata.produced_at for this gate; tests use coherent metadata/payload dates.
No sleeps or wall-clock advancement change the frontier. Required handler validation
rejects incomplete sets. Malformed event JSON is retried, not turned into an invented
fill (the service/transport owns quarantine policy).

### R18 — Backend restart releases local binding, retaining pending durable deliveries

Publish Fill101 while the frontier blocks D3, poll/defer it, destroy BackendGateway,
then recreate it with the same stream/durable name and open frontier. Fill101 must
still arrive exactly once after success. Acknowledged earlier deliveries must not
reappear as new economic messages on restart. No submit command is manufactured
by restoring a consumer binding.

### R19 — Snapshot routing preserves signed cash/positions and partial open orders

A backend snapshot at D2 containing cash798, BTC+2, collateral50 and open BUY5
filled2 reaches on_snapshot unchanged, with its metadata ID/correlation. A snapshot
request retains the caller's identity but does not dictate the venue's timestamp.
Future snapshot metadata is subject to the same frontier deferral as fills. Snapshot
messages use a separate snapshot consumer; they cannot be decoded as executions.

### R20 — Backend invalid commands/configuration fail explicitly

Zero order IDs, empty message IDs, empty snapshot request ID and nonpositive poll
timeout reject before publishing an economic command. An empty stream name cannot
create a usable route (connection construction may occur before validation). The
adapter never substitutes the canonical MOCK facade or another backend on failure.
Example: cancel ID0 cannot become a cancel-all; a malformed event cannot become a
BUY2 or synthetic success. Tests of this boundary require the real temporary NATS
service, since the public constructor owns JetStreamBus.

## Invariants and coverage boundaries

- Every fill corresponds to a submitted active order; sizes/sides/prices/fees retain units.
- Commands/status/snapshot requests are not cash or position mutations.
- Explicit dates govern eligibility; polling is technical delivery only.
- Retry identities and recovered FillIDs remain stable; duplicate handling belongs to
  the boundary that explicitly owns it, not an invented adapter cache.
- Unit tests use only deterministic local objects. Backend service tests are written
  but not run in steps1–2 and always skip without Docker.
- Real HTTP exchanges, gateway service main loops, production routing and database
  outboxes belong to other areas. Canonical MOCK tests here exercise adapter surfaces.

## Findings

Step 3: `ninja -C build -j2` completed without test compiler warnings. Both adapter
executables were run directly with `ALGOTRADING_SOURCE_ROOT=$PWD`. Of 54 tests,
44 unit tests passed, 2 unit tests are disabled for the findings below, and all
8 service tests skipped because Docker is unavailable. BackendGateway behavior
therefore remains unvalidated in this environment.

1. **R4 — Malformed mutable market intent reaches the wire.**
   `MessageExchange.DISABLED_R4_MalformedMutableMarketIntentCannotReachWire` retains
   the assumed defensive rejection rule. Starting with a valid BUY2 DTO, each
   independent mutation to quantity 0, -1, NaN or infinity, or an empty asset,
   submitted without throwing and published a command. All five rejection and
   empty-publication assertions failed. `MessageExchange::submitOrder` checks only
   the order ID; the market-order JSON encoder does not revalidate quantity/asset,
   whereas decoding constructs a validated ExecutionOrder. This can publish
   commands the receiver cannot execute. Expected rejection values are unchanged.

2. **R13 — Canonical partial status is published twice.**
   `CanonicalAdapter.DISABLED_R13_OrderedStreamPublishesFillBeforeItsFeeAndPartialStatus`
   received four events for the worked BUY0.2 example at timestamp20: Fill0.1 at100,
   fee -0.00400000, then two PartiallyFilled updates for order1/cumulative0.1.
   Fill, fee, identity and partial-status assertions passed; the expected one
   lifecycle update and three total events failed with actual counts two/four.
   `MockRecovery::wireHandlers` captures lifecycle output directly, and
   `MockMatching::forwardLifecycleTransition` forwards the same update through
   the matcher handler, which recovery captures again. The public facade forwards
   both stream entries. Expected counts remain one update/three events.
