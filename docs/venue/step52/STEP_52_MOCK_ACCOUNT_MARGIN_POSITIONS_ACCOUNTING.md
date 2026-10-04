# Step52 — Mock Account / Margin / Positions / Accounting

**Entering state:** Step51 PASS/CLOSED.  
**Bound Step51 matching/fill fingerprint:** `d06a71a4cbc7080cd4c05f7d5b3bc84ec06b55138d25590b8eefbb251e7c4720`

Step52 adds the deterministic MOCK economic account projection. Canonical `Fill`
events are the sole execution authority for position changes.

## Economic model

MOCK v1 is a synthetic linear perpetual account:

- initial settled cash: `100000.00 USD`;
- fills do not move principal notional cash;
- closing fills realize PnL into settled cash;
- every accepted fill posts the Step51 `400 ppm` trading fee as a canonical
  `TRADING_FEE` accounting event;
- explicit positive rebates post `REBATE`;
- explicit signed funding inputs post `FUNDING_PAYMENT`;
- equity = settled cash + unrealized PnL;
- margin used = absolute marked position notional / configured integer leverage;
- available margin = max(0, equity - margin used).

This is a deterministic MOCK accounting convention, not a claim about any real
venue's liquidation or margin engine.

## Positions

Positions are keyed by explicit canonical MOCK market identity.

Same-direction fills update average entry. Opposite-direction fills:
- realize PnL on the closed quantity;
- preserve average entry on a partial close;
- reset average entry at the fill price if the fill flips the position;
- remove average entry when flat.

Marks are supplied explicitly with business/event timestamps. No wall clock is
read for economics.

## Fixed-point arithmetic

Price, quantity and money use the Step49 8-decimal grids. PnL, notional and fees
are calculated with checked fixed-point helpers. The engine does not silently
round an off-grid fill or mark onto the venue grid.

## Fill idempotency

`native_fill_id` is the in-process economic dedup key.

- same ID + same exact fill fingerprint -> duplicate ignored;
- same ID + conflicting economics -> accounting marked unsafe and the conflicting
  fill is rejected.

Durable deduplication across restart is intentionally deferred to Step53.

## Accounting events

Trading fee:
- generated automatically from canonical Fill;
- negative signed amount;
- correlation `fee:<native_fill_id>`.

Rebate:
- explicit positive amount;
- explicit event ID.

Funding:
- explicit signed account-centric amount;
- explicit event ID.

External accounting event IDs are also idempotent in-process. Conflicting reuse
marks accounting unsafe.

## Account / margin views

Step52 provides:
- canonical `AccountSnapshot`;
- balances;
- signed positions;
- average entry;
- explicit mark;
- realized PnL;
- unrealized PnL;
- equity;
- gross exposure;
- leverage;
- margin used;
- available margin;
- deterministic economic fingerprint.

## Order views

Open orders and the known-order archive are derived from Step50 lifecycle state.
They are read projections only; they do not create fills or mutate order state.

## Important exclusions

Step52 does not implement:
- restart persistence;
- snapshot + incremental user stream;
- reconnect/backfill;
- duplicate/out-of-order recovery across restart;
- real venue reconciliation;
- private Hyperliquid auth/signing/routing;
- liquidation engine.

Those boundaries remain fail-closed.

## Next

**Step53 — Mock Snapshot / User Stream / Recovery**
