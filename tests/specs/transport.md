# Transport and service message contracts

## Purpose and sources

Carry trading intent and actual execution evidence without changing money, signs,
identity or business time. JSON is a representation boundary; JetStream provides
durable delivery and explicit dispositions. Neither boundary fills an order or
guarantees that a downstream account has applied an event only once.

Sources: `docs/codex/ARCHITECTURE_FULL.md` (Durability, business versus technical time),
`lib/README.md` (Messaging), `live_trading/README.md` (Durability and message handling),
public headers under `lib/src/transport/`, and the DTO/comment contracts in
`lib/src/contracts/`. `VenueContracts::V1` adapter objects and manual admission have
no `MessageJson` entrypoints; their adapter/runtime behaviours belong to those areas.
This specification covers all 21 encode/decode pairs declared by `message_json.h`.
Where a safety expectation goes beyond an explicit public statement, it is labelled
`(assumed)`. No finding is inferred merely from reading implementation code.

## JSON rules

**R1 — Every supported message round-trips all its fields.** For each of the 21 types,
decode(encode(message)) equals the input by field value, including nested metadata,
maps, arrays, partial fills and optional intent. Map iteration order and JSON object
key order are not business semantics. Strings are opaque and preserve quotes,
newlines and UTF-8. Worked examples used throughout the tests:

| Message | Worked input and preserved meaning |
| --- | --- |
| MarketDataReleaseRequest | ExecutionOpen at 20260103, decision 20260102; no change to either date. |
| MarketDataUpdated | completed_through 20260102; counts ranked 100, top 10, tracked 12, maintained 14, requested 4, downloaded 8; 4 symbols x 2 rows = 8 rows. |
| MarketSliceClosed | 20260102, symbols BTC and ETH; one complete slice, not two decisions. |
| MarketSliceSnapshot | BTC OHLC 70/80/65/75, volume 10, quote turnover 720; ETH 95/110/90/100, volume 20, quote turnover unavailable. |
| StrategyIntentBatch | strategy 7 signals BTC +1, ETH -0.5; missing SOL stays absent/zero, no sizes added. |
| DecisionBatch | close 20260102, capital 1000, BTC weight .25 -> target USD 250; ETH Flat -> target 0; explicit SOL Hold stays Hold. |
| ExecutionPriceSnapshot | open 20260103, originating decision 20260102; BTC 125, ETH 100. |
| SubmitOrderCommand | order 41, strategy 7, Buy 2 BTC, created 20260102, active 20260103; USD 250 / open 125 = 2. |
| CancelOrderCommand | cancel order 40 requested 20260103; never implies its remaining quantity was canceled already. |
| OrderUpdateEvent | order 40 PartiallyFilled at 20260103 with external ID and explanatory text; update is not a fill. |
| FillEvent | fill 4294967297, order 41, Buy 2 at 75, commission 1; cash effect -(2*75)-1 = -151. |
| VenueCostEvent | funding -2.5 USD on 20260103 with stable cost ID; cash decreases 2.5, quantity unchanged. |
| AccountSnapshot | cash 849, BTC +2, ETH -.5; marks 75/100 give equity 849+150-50 = 949; collateral 500 is separate from cash; strategy attribution and resync flag preserved. |
| ExecutionCycleComplete | decision 20260102, execution 20260103, revision 9; preserve the quiescence evidence. |
| ExchangeSnapshotEvent | cash 849, collateral 500, signed positions; open order 40 Buy 5, filled 2 -> remaining 3. |
| ExchangeSnapshotRequest | all four metadata fields, including producer-owned message ID and correlation. |
| OrderPlanningRequest | decision close T, executable prices T+1, revision 9, next ID 41, tracked partial order 40 and both strategy positions. |
| OrderPlanBatch | revision 9, next ID 42, cancel 40, submit 41; preserve nested close decision and open prices plus USD audit targets. |
| DailyCloseSnapshot | date 20260102, BTC 75, ETH 100; never substitute BTC open(T+1)=125. |
| NotionalOrderPlanningRequest | close 20260102, revision 9, state and immutable close prices; no venue quantity appears. |
| NotionalOrderPlanBatch | target USD 250 minus held USD 150 minus pending USD 25 = Buy USD 75; reference close 75; economic ID planned-notional-order:20260102:9:41, next ID 42. |

Metadata examples use schema 1, nonempty logical/correlation IDs and produced_at
20260102. Numeric identities must retain their integer precision; fill 4294967297
must not become 1. Finite doubles such as quantity .12345678901234566 must survive
exactly as the original binary double, not be truncated to a display precision.

**R2 — Reject malformed JSON and wrong top-level shapes.** Every decoder throws on
invalid syntax, JSON null/scalars/arrays instead of the message object, or an object
missing metadata. Example: `{`, `null`, `[]`, `42`, `"fill"`, `{}` cannot invent a
fill ID or interpret missing cash as 0. Rejection is an exception; no downstream
state transition is part of decoding.

**R3 — Required fields are not invented; extensions remain compatible.** All four
metadata keys and all mandatory DTO fields must be present with their declared
JSON types, also in nested orders/fills/plans. Example: order 41 without quantity,
fill 4294967297 without commission, or a planning request without state_revision
throws instead of supplying 0. Tests explicitly enumerate required keys, rather
than treating every emitted optional field as required. Objects must not be accepted
as arrays of strategies/bars/orders (assumed shape safety). Unknown extension fields
may be ignored (assumed forward compatibility); adding `producer_note` to a valid
message leaves its fields intact. Schema acceptance beyond explicit wire validators
belongs to consumers; these tests do not impose a uniform schema policy on all DTOs.

**R4 — Preserve documented optional/default representations.** Old market bars without
quote_volume mean unavailable (NaN), never quote turnover 0; explicit null likewise
means unavailable. A finite quote_volume of 0 is known zero. Legacy market update
counts default to 0. Absent account collateral/resync default to 0/false; absent
order reference price/immediate flag/position stop default to 0/false/0; absent
target_notional_usd means an empty map. Legacy execution-price/release messages may
omit decision_timestamp, giving 0. Example: old BTC volume 10 without quote turnover
must not be interpreted as USD 750 merely because its close is 75. Default optional
fields are omitted where their public compatibility comments require byte stability.

**R5 — Signals, fills and costs preserve valid money and reject invalid numeric
payloads.** Signals must be finite and within [-1,+1], with nonempty symbols; +1 and
-1 are valid. A fill requires a nonempty symbol, quantity > 0, price > 0 and fee >= 0.
Example: Buy 2 at 75, commission 1 -> cost 151; quantity 0, price -75, or commission
-1 must throw. Selling 2 at 75 with fee 1 instead returns 150-1 = 149 cash.
Venue collateral must be finite and nonnegative: -500 or JSON null cannot be used
as collateral. Paid funding -2.5 and received funding +2.5 retain their signs.
FEE/FUNDING/INTEREST/CAPITAL_FLOW round-trip as distinct categories; unknown cost
kind, empty cost ID, zero timestamp or nonfinite cost amount must be rejected.
Example: deposit +100 is capital, not a trading profit; fee -1 spends 1 USD.

**R6 — Unsupported research conditions are rejected, never silently made market
orders.** Service v1 rejects entry_stop_price, protective_stop_price and parent_order_id
when nonzero on encode and decode, including nested planning state/output. Likewise
research triggers on decision intents are rejected. Example: BTC Buy 2 conditional
on trigger 110 must not be transmitted as unconditional Buy 2. Zero-valued legacy
fields may be read without adding a condition.

**R7 — Supported manual and LIVE position-stop intent round-trips safely.** Manual
Buy 2 with reference_price 75 and fill_at_reference_price true preserves both; a
reference-fill order without a positive reference price is rejected on decode.
LIVE Sell 2 with position_stop_price 60 remains a stop, distinct from a research
bracket. Invalid Buy position stop, negative stop price or a position stop combined
with the reference-fill flag is rejected. Example: 60 is a trigger, not the order's
fill price and not quantity 60.

**R8 — Notional plans bind identity, revision, time and economics.** Plan identity is
`notional-order-plan:20260102:9`; order identity is
`planned-notional-order:20260102:9:41`. Nested decisions and reference closes must
refer to the same T; each order must use the plan revision and exact reference close.
Buy delta +75 requires side Buy and notional 75; Sell delta -75 requires side Sell
and notional 75. Price 0, size 0, unknown side, mismatched identity/revision/reference
price and inconsistent absolute delta/size are rejected. Example: replacing close
75 by open 125 on only the order makes the plan inconsistent, not a new valid price.
Repeated encoding/decoding must not generate new identities or advance revisions.

**R9 — Duplicate nested identities cannot silently discard exposure (assumed where
not explicitly documented).** Duplicate strategy-position entries in quantity and
notional planning requests throw. Duplicate per-asset decisions likewise throw,
since choosing one hides a potentially different monetary target. Example: strategy
7 twice with BTC 2 and BTC 3 cannot recover arbitrarily as BTC 2; BTC TargetWeight
.25 followed by Flat cannot silently choose the first. This is codec ambiguity
rejection, not business-level message deduplication.

**R10 — Separate close(T), open(T+1), and operational processing time.** The codec
preserves caller timestamps; it never consults the wall clock or changes dates on
redelivery. Example: close 20260102 at 75 and execution open 20260103 at 125 remain
separate inside a nested planning message; target USD 250 is still 250, while the
executable order has 2 units. A plan is not an execution: its JSON carries no extra
fill or cash mutation. Validation of release chronology and freshness belongs to
the relevant runtime unless explicitly enforced by the wire contract.
ClosedSlice at 20260102 carries decision_timestamp 0, whereas ExecutionOpen at
20260103 carries decision_timestamp 20260102; unknown release kind 99 is rejected.

## Routing rules

**R11 — Subject strings and stream ownership are protocol.** Public subjects retain
their documented `.v1` names. Runtime stream has exactly 19 trading subjects;
gateway-control owns `execution.exchange.snapshot.request.v1` separately; private
backend owns six `gateway.backend.*` subjects. Lists contain no duplicates or
cross-owned subjects. Example: submitting `execution.command.submit.v1` cannot be
misrouted to `gateway.backend.command.submit.v1` by a refactor. `runtimeSubjects()`
remains the same trading subject set, with no clock-control subjects.

**R12 — The execution-event wildcard is deliberately bounded.**
`execution.event.>` covers order updates, fills and venue costs, not submit/cancel,
account snapshots, cycle barriers or gateway backend events. Example: one fill on
execution.event.fill.v1 reaches an execution-event consumer; one submit on
execution.command.submit.v1 does not. A wildcard durable must rebind after restart
without resetting its acknowledgement progress.

## JetStream durability rules

All following cases use a real throwaway `nats:2.10-alpine -js` container, a separate
service-suite executable, the prescribed Docker availability skip, and timeout 300.
No venue, public host or external broker is contacted. Tests wait using bounded pull
I/O, never sleeps. AckWait is broker/technical time, not economic TimeHandler time.

**R13 — Persist published messages before a consumer exists.** A later durable
subscriber receives retained messages with exact subject and payload, including
binary-safe bytes. Example: publish a fill payload containing one embedded NUL,
then subscribe -> one delivery with the same length and bytes, not a truncated fill.
Publishing only intent changes no account state.

**R14 — Broker deduplication uses the logical message ID.** Within the broker's
deduplication window, publishing payload A twice with ID fill-101 yields one retained
logical message; publishing the same bytes with ID fill-102 yields a second. Example:
two duplicate deliveries of an intended Buy 2 may not become two separate retained
broker messages solely due to a producer retry. Consumers still need fill-ID
deduplication after unacknowledged redelivery; transport alone is not exactly-once
accounting.

**R15 — Ack follows handler completion and survives local close/restart.** Once a
handler has durably handled a message and returned Ack, a new binding to the same
durable does not replay it. close() releases the local binding, preserving the server
consumer and ACK floor. Example: deliver fill-101, Ack, close/recreate bus, publish
fill-102 -> only fill-102 reaches that durable. An independent durable has its own
progress and can still receive fill-101. Both exact subjects and execution.event.>
must retain this property.

**R16 — Retry requests redelivery rather than successful completion.** First handler
call returns Retry; a later call for the same message returns Ack. Example: a
temporary database rejection for fill-101 yields two handler invocations with
identical subject/payload, then no third delivery. poll() counts completed handler
dispositions, not successful economic applications only.

**R17 — Terminate stops that consumer's retries without deleting other messages.**
Example: invalid message 101 returns Terminate; after durable rebind it does not
return, but a subsequent valid message 102 still reaches the handler and is Acked.
Another durable may still inspect the retained invalid message; termination is not
a stream purge.

**R18 — No-ack failures remain recoverable, including the remainder of a fetched
batch.** A handler exception propagates; neither that delivery nor untouched fetched
messages are acknowledged. After closing/rebinding and AckWait, they redeliver.
Example: pull messages 101 and 102, throw while handling 101 -> a replacement
handler receives both, with 102 never lost. A single-message crash-after-commit
simulation has the same redelivery boundary, and downstream IDs own economic
idempotency.

**R19 — Stream setup is idempotent and additive.** Repeated ensureStream retains
existing messages/subjects; adding the cost-event subject must not remove the fill
subject. Example: create fill stream, publish fill-101; ensure [fill,cost] twice,
publish cost-102 -> both are delivered once. Broker/container restart preserves
file-backed retained messages and durable ACK progress after reconnecting a new bus.

**R20 — Invalid local consumer/poll configuration fails explicitly.** Empty stream,
durable or subject, a dotted durable name, nonpositive AckWait/MaxDeliver/MaxAckPending,
an empty handler, nonpositive poll limits/timeouts, or an unknown local subscription
ID must throw. Example: batch size 0 may not be reported as a successful empty poll.
Rebinding a durable to a different filter fails and preserves its old progress.
An idle valid poll returns 0; closing an already closed local subscription is safe.

## Findings

### F1 — Object-shaped collections silently become empty vectors (R3)

`MessageJsonBehaviour.DISABLED_R3_CollectionsCannotBeReplacedByObjects` preserves
the expected rejection from the assumed shape-safety rule R3. On the first run,
each of these replacements decoded without an exception:

- `MarketSliceSnapshot.bars = {}`
- `StrategyIntentBatch.strategies = {}`
- `DecisionBatch.strategies = {}`
- `OrderPlanBatch.submit_orders = {}`
- `NotionalOrderPlanBatch.submit_orders = {}`

The corresponding loops in `lib/src/transport/message_json.cpp` iterate the supplied
JSON without checking that it is an array. Iterating an empty object executes zero
iterations, so malformed market/strategy/order collections are accepted as empty
business state. The public DTOs declare vectors, and R3 explicitly requires array
shape. This is a code/spec disagreement; the assumed rule and expectations are
unchanged. The test is retained with `DISABLED_` and `// FINDING:`.

### F2 — Conflicting decisions for the same asset are silently discarded (R9)

`MessageJsonBehaviour.DISABLED_R9_DuplicateAssetDecisionCannotSilentlyChooseOneExposure`
preserves the expected `std::invalid_argument`. The input contains the worked
example's BTC TargetWeight .25 (USD 250 from capital 1000), followed by another BTC
decision Flat (weight 0). The first run returned normally instead of rejecting the
ambiguity. `decodeDecisionBatch` inserts decisions using `unordered_map::emplace`
without checking whether insertion succeeded, keeping the first BTC decision and
silently dropping the conflicting Flat. This disagrees with the assumed ambiguity
rejection rule R9. Neither the spec rule nor the expected exception was changed;
the test remains disabled with its finding comment.

### Step 3 validation

- `ninja -C build -j2` compiled both transport executables without warnings.
- Initial in-process run: 45 cases, 43 passed, 2 failed (F1 and F2). All 21 message
  round trips, all required-field rejection checks, and all three subject tests passed.
- JetStream executable: all 13 cases skipped with `Docker is not available`; real
  NATS durability/disposition behaviours remain unverified here. Skips are not passes.
- After triage, rebuilt with `ninja -C build -j2` and ran
  `meson test -C build --no-rebuild --print-errorlogs unit_transport unit_transport_jetstream`.
  Final result: 58 registered cases = 43 passed, 2 disabled findings, 13 Docker
  skips, 0 remaining failures. Meson reports both executables OK; the JetStream
  result represents skips rather than broker-backed passes. Final output is in
  `build/meson-logs/testlog.txt`. No test expectation or production code was changed.
