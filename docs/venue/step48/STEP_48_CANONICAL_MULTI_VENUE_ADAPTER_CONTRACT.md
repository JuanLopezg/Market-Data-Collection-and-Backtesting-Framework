# Step48 — Canonical Multi-Venue Adapter Contract v1

**Entering state:** Step47A + 47B + 47C PASS/CLOSED.  
**This step:** implement the common C++ interface and canonical DTO vocabulary.  
**This step does not:** implement a concrete exchange, private authentication,
signing, private order routing, capital movement or smart-order-routing.

## Why the Step47A headers are not edited

The Step47A headers are part of a frozen/hash-checked historical architecture
baseline. Step48 therefore leaves them untouched and introduces the stable v1
namespace:

`VenueContracts::V1`

This keeps the old gates reproducible while turning the Step47C freeze into the
real adapter contract.

## Interface

`lib/src/exchange/canonical_venue_adapter.h`

Concrete future implementations all implement the same interface:

- MockExchangeAdapter — Steps49–55.
- HyperliquidAdapter — private integration after Step62.
- FutureVenueAdapter_X — later, without rewriting Strategy/Risk/Planner.

The adapter exposes:
- explicit `VenueContext`;
- versioned/explicit capability set;
- one ordered canonical event stream;
- submit/cancel/modify batches;
- account snapshot;
- open orders;
- order status;
- fill/backfill request;
- technical `poll()` only.

There are deliberately **no** methods for keys, signing, protocol URLs or wallet
management in the canonical interface.

## Identity

Canonical market identity is:

`canonical_asset + product_class`

with product classes:
- PERPETUAL
- SPOT
- OUTCOME
- OTHER

Venue symbols/native asset IDs are explicit per-venue locators. They are not
canonical identity and must never be inferred heuristically.

## Capability-driven behavior

Core code is allowed to ask:

`adapter.capabilities().supports(...)`

It is not allowed to implement behavior based on concrete venue identity.

A required missing capability means:

`BLOCK_ROUTE_FOR_THAT_VENUE`

It never means fallback to another venue.

## Orders and idempotency

The contract separates:
- durable canonical `request_id`;
- local internal order ID;
- optional venue client-order ID;
- native venue order/fill references.

`client_order_id` is never treated as proof of native idempotency.

For ambiguous submit outcomes:

`UNKNOWN_REQUIRES_RECONCILIATION`

and no blind resubmit is allowed unless a concrete adapter explicitly advertises
`IDEMPOTENT_SUBMIT` with semantics that support it.

## Result envelope

Batch actions distinguish:
- OPERATION-level rejection before item evaluation;
- ITEM-level results.

A concrete adapter must never invent item results when the venue rejected the
whole operation before evaluating items.

## Accounting

The canonical adapter stream contains accounting events:
- TRADING_FEE
- REBATE
- FUNDING_PAYMENT

Their amount is a decimal string. Sign convention:
- positive => increases account value;
- negative => decreases account value.

## Existing runtime

Step48 does **not** replace or wire the current `ExchangeGatewayAdapter` runtime
path yet. It remains untouched as the existing gateway transport boundary.

The new canonical interface is the contract that MockExchange will implement
starting in Step49. Runtime migration/wiring occurs in the later full-system
replay steps, rather than being silently mixed into this contract step.

## Next step

Step49 — Mock Venue Catalog & Trading Rules.

Step49 will provide the first concrete venue-specific leg behind this contract:
MOCK symbol/catalog identity, tick/lot/price/size precision, minimums and relevant
margin/leverage rule metadata, all versioned/fingerprinted and with no heuristic
symbol mapping.
