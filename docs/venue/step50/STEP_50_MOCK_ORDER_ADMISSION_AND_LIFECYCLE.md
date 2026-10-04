# Step50 — Mock Order Admission & Lifecycle

Entering state: Step49 PASS/CLOSED.

Bound Step49 venue fingerprint:
`a6d2b98a45d11e57bc0d649ff9569952c73adff1671608f5eafe561169ccbb27`

Step50 implements deterministic submit/cancel/modify admission and the MOCK
order lifecycle state machine. It does not yet implement matching, fills,
slippage, fees/funding, positions/PnL, restart recovery or private real-venue
routing.

## Implementation boundary

The new component is:

`MockVenueV1::MockOrderAdmissionLifecycleV1`

It is intentionally not yet the final `MockExchangeAdapter`. The Step48 adapter
interface also exposes account/fill/recovery surfaces that belong to Steps51–53.
Creating a partial adapter now would force fake/no-op semantics.

## Admission

An order is admitted only when its venue context, canonical identity, product
identity, venue symbol, native asset ID and rule profile match Step49 exactly.

It then enforces:
- price increment;
- size increment;
- minimum size;
- minimum notional;
- supported GTC/IOC;
- supported post-only/reduce-only flags;
- post-only + IOC incompatibility;
- unique local order ID;
- historical uniqueness of a non-empty client order ID.

No mapping heuristics are added.

## Decimal behavior

Step49 rule values are base-10 strings. Step50 converts the shortest round-trip
decimal form of the canonical double to fixed-point units and rejects off-grid
values rather than silently rounding them.

Minimum notional comparison is performed in fixed-point integer units.

## Idempotency

`request_id` is the deduplication key across all command types.

Same request ID + same payload:
- cached operation result is returned;
- no duplicate state mutation;
- no duplicate lifecycle event.

Same request ID + different payload:
- operation-level `DUPLICATE_REQUEST`.

Restart persistence of this request cache is Step53.

`client_order_id` remains separate and does not replace request ID.

## Lifecycle

Submit:
`ACCEPTED`

It does not become RESTING yet, because Step50 has no matcher.

Cancel:
`CANCEL_PENDING -> CANCELED`

Modify:
- requires active order;
- cannot change instrument identity;
- re-runs admission;
- preserves local/native order identity and current lifecycle phase.

A constrained lifecycle bridge is exposed for Step51. It can validate state
transitions, but it cannot create a Fill or economic execution by itself.

## Deliberately deferred

- post-only crossing decision -> Step51;
- IOC remainder cancellation -> Step51;
- reduce-only position/economic check -> Step52;
- restart durability/user-stream recovery -> Step53.

## Historical gates

Step48 and Step49 contained anti-future assertions that proved Step50 did not
exist when those steps closed. Step50 therefore verifies their frozen hashes
instead of rerunning those historical anti-future assertions.

## Next

Step51 — Deterministic Matching / Fill Model.
