# Step47C — Hyperliquid v1 Scope Freeze

This file freezes **scope**, not private integration.

## First real venue
`HYPERLIQUID`

## V1 product scope
- **IN_SCOPE:** default Hyperliquid perpetuals.
- **DEFERRED:** HIP-3 / builder-deployed perpetuals.
- **DEFERRED:** spot.
- **OUT_OF_SCOPE:** outcome markets.

The canonical architecture still knows that PERPETUAL, SPOT, OUTCOME and OTHER
product classes can exist. Restricting Hyperliquid v1 does not make the core a
perpetual-only architecture.

## V1 execution semantics
In scope for the canonical adapter contract and later Hyperliquid implementation:
- limit orders;
- GTC;
- IOC;
- post-only/ALO;
- reduce-only;
- cancel;
- modify;
- optional native client order ID (`cloid`) as a venue-native correlation aid.

The existence of `cloid` does **not** freeze a native idempotency guarantee.

## Deferred venue features
- trigger / TP-SL order execution;
- TP/SL grouping;
- TWAP;
- HIP-3 execution;
- spot execution.

These may be represented by capabilities/extensions without becoming mandatory
for other venues.

## Out of scope for trading adapter v1
- transfers;
- withdrawals;
- staking;
- vault capital actions;
- other capital movement.

## Private boundary
Private Hyperliquid authentication/signing is still deferred until after:
Step48–61 -> Step62 revalidation/parity -> historical Step37.

Nothing in Step47C authorizes a private key, order submit or capital movement.
