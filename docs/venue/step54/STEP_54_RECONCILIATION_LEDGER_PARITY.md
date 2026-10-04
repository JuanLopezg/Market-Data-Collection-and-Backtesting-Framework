# Step54 — Reconciliation + Ledger Parity

**Entering state:** Step53 PASS/CLOSED.  
**Bound Step53 recovery fingerprint:** `d7801a18523cf93e61f277373b2e2190070137f74ada03135fc79d1d5005dfdc`

Step54 closes the MOCK safety loop between a local expected projection and the
recovered MOCK venue truth. It also adds a deterministic SHA-256 economic ledger
projection over canonical `Fill` and `AccountingEvent` evidence.

## Reconciliation states

Exactly three states exist:

- `CLEAN` — evidence is fresh, complete and all compared dimensions match.
- `BLOCKED` — fresh evidence contains an economic/identity contradiction, the
  ledger chain is invalid, or recovery is unsafe.
- `PENDING` — venue evidence is missing/invalid or local and venue evidence are
  from different stream sequences.

`PENDING` never becomes a synthetic PASS.

## Freshness

The local expected projection is rebuilt from the deterministic Step53 canonical
user stream. The venue truth is the Step53 state snapshot.

A comparison is current only when:

`local.stream_sequence == venue.snapshot_sequence`

A mismatch is `PENDING`, not `CLEAN` and not a guessed contradiction.

## Dimensions compared

### Settled cash

The Step54 ledger derives settled cash from:

- initial cash `100000.00`;
- realized PnL on closing canonical fills;
- `TRADING_FEE`;
- `REBATE`;
- `FUNDING_PAYMENT`.

That exact fixed-point result must equal the venue `USD` balance total.

### Positions

For each non-zero position Step54 compares:

- exact canonical + venue instrument identity;
- signed quantity;
- average entry price.

The expected position is independently rebuilt from canonical fills.

### Open orders

The local expected open-order projection is rebuilt from canonical `OrderUpdate`
events and compared against Step53 `OpenOrdersSnapshot` for:

- local order ID;
- exact instrument identity;
- lifecycle status;
- cumulative filled quantity;
- remaining quantity;
- native order ID;
- native client-order ID.

Side and limit price are deliberately not reconstructed from `OrderUpdate`
because those fields are not present in that canonical event. Step54 does not
invent them. A later full-system local expected state may add richer parity from
the ExecutionState order intent.

### Fills

The complete local canonical fill set must exactly match the Step53 venue fill
snapshot by native fill ID and canonical/native identity, order/strategy ID,
event time, side, quantity and price.

## Ledger

Every economic stream event produces an immutable deterministic ledger entry.

Entry kinds:

- `FILL`
- `TRADING_FEE`
- `REBATE`
- `FUNDING_PAYMENT`

IDs:

- fill: `fill:<native_fill_id>`
- accounting: `accounting:<correlation_id>`

Each entry hashes:

`previous_hash | sequence | kind | identity | fixed-point economics`

with SHA-256. Genesis is 64 zero hex characters.

A fill ledger entry records:

- signed position delta;
- gross notional;
- realized PnL;
- fill cash delta = realized PnL only.

Principal notional does not move settled cash in this synthetic linear perpetual
model.

Every fill must have exactly one canonical trading-fee event with the exact
Step51/52 `400 ppm` amount. Rebate and funding events preserve the signed
account-centric Step48 convention.

## Ledger parity

A valid chain alone is insufficient. The final ledger projection must also match
venue truth:

- settled cash;
- non-zero position quantities;
- average entry prices;
- canonical fill set.

Thus a valid-looking chain whose economics disagree with venue state is
`BLOCKED`.

## New-order safety gate

Step54 introduces an explicit new-submit gate.

A new submit is permitted only if all are true:

1. last reconciliation is `CLEAN`;
2. the reconciled sequence equals the venue's current sequence;
3. Step53 recovery is still safe.

As soon as any venue/user-stream state advances, that prior reconciliation is
stale and new submits are blocked until reconciliation runs again.

This gate is specifically for **new orders**. Cancel/modify safety policy is not
redefined here; recovery/risk controls must still be able to reduce risk.

## Restart

Because Step53 reconstructs the exact deterministic source stream after restart,
Step54 must reproduce:

- `CLEAN`;
- the same economic fingerprint;
- the same SHA-256 ledger head;
- the same fill/order/cash/position parity.

The validation campaign proves this.

## Intentionally not compared in Step54 v1

Step54 does not compare:

- mark price;
- unrealized PnL;
- equity;
- margin used;
- leverage.

Those values exist in Step52 venue state, but Step53's canonical incremental
user stream currently has no mark/leverage event from which an independent local
expected projection can derive them. Omitting them is safer than fabricating an
independent value.

## Still deferred

Step54 does not implement:

- fault injection / rate-limit behavior;
- real venue reconciliation;
- Hyperliquid private auth/signing/routing;
- smart/split routing;
- live capital.

## Next

**Step55 — Fault / Chaos / Rate-Limit Engine**
