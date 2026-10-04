# Step47C — Portability / Architecture Freeze Gate

**Entering state:** Step47A PASS and Step47B PASS.  
**Result of this step:** freeze the venue-neutral semantics that Step48 will
implement. Step47C does not implement the adapter itself.

## Final architecture rule

Hyperliquid is the first real venue, not the architecture.

The core is allowed to branch on **capabilities and canonical state**, not on
concrete `venueId` strings. `venueId/environment` are identity/evidence/routing
data, not Strategy/Risk/Planner behavior switches.

## Resolutions

### 1. Product identity
Canonical asset alone is insufficient because the same economic symbol can
represent spot, perpetual or another product. V1 therefore freezes:
- `canonicalAsset`;
- `productClass` = PERPETUAL / SPOT / OUTCOME / OTHER;
- optional quote/settlement asset;
- optional neutral contract variant.

The venue-native symbol/asset ID remains an explicit registry leg scoped by
venue/environment.

### 2. Order lifecycle
V1 freezes:
`PENDING_SUBMIT -> ACCEPTED/RESTING -> PARTIALLY_FILLED -> FILLED`
with cancel/reject branches and an explicit
`UNKNOWN_REQUIRES_RECONCILIATION` state.

Unknown/ambiguous submit outcome is safety-relevant: no blind resubmit.

### 3. Idempotency
Three concepts are kept separate:
- canonical durable request ID;
- optional venue client-order ID;
- proven native idempotency capability.

A client order ID is not automatically an idempotency guarantee. If submit
outcome is ambiguous and native idempotency is not proven, reconcile before any
resubmit.

### 4. Trigger orders
`TRIGGER_ORDERS` enters the neutral capability vocabulary, but it is **not**
required for current PureRSI v1 and is deferred for Hyperliquid v1.

### 5. Fees and funding
V1 adds neutral `FEE_ACCOUNTING` and `FUNDING_ACCOUNTING` capabilities and
accounting event types:
- TRADING_FEE;
- REBATE;
- FUNDING_PAYMENT.

Signed amount convention is account-centric:
positive increases account value, negative decreases it.

### 6. Batch results
V1 distinguishes:
- operation-level rejection before item evaluation;
- item-level results.

The adapter must never fabricate per-item venue evidence when the venue rejected
the whole operation.

### 7. Reject taxonomy
The final v1 canonical reject taxonomy extends the Step47A baseline with
`VENUE_LIMIT`. Native code/reason remain preserved. Local PortfolioRisk rejection
is **not** a venue error.

### 8. Native fill identity
The core treats `native_fill_id` as opaque. Every adapter must construct a
stable unique reference from whatever venue evidence is sufficient. No single
venue-native field is assumed globally unique by the core.

### 9. Hyperliquid v1 product scope
The first private Hyperliquid implementation is frozen to **default perpetuals**.
HIP-3, spot, triggers/TP-SL and TWAP are deferred; outcomes and capital movement
are outside v1.

## Portability proof

A hypothetical `FUTURE_VENUE` deliberately lacks modify, client-order IDs,
post-only, batching, leverage, user stream, dead-man switch and funding.

It still fits the same architecture:
- adapter declares capabilities;
- a route that requires a missing capability is blocked for that venue;
- no fallback occurs;
- Strategy/Risk/Planner contracts are unchanged;
- the same dashboard/read models remain valid.

Therefore “multi-exchange-ready” means extensible by adapter, **not** that every
venue supports every feature.

## What Step48 must do

Step48 will implement the canonical interface from the frozen JSON artifacts.
It must not:
- enable Hyperliquid private auth/signing;
- place orders;
- add smart-order-routing;
- add silent venue fallback;
- branch core Strategy/Risk/Planner behavior on `venueId`.

Step47C is complete only if its gate proves the Step47A/47B inputs remain intact,
all recorded gaps are resolved explicitly, FUTURE_VENUE portability is preserved,
and the next step is Step48.
