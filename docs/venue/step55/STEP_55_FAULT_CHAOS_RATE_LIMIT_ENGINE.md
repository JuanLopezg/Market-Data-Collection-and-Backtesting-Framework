# Step55 — Fault / Chaos / Rate-Limit Engine

**Entering assumption for this delivery:** Step54 PASS/CLOSED.  
**Bound Step54 reconciliation/ledger fingerprint:** `8ea5f659e3bdb3f57c2e0a3408a8ab4a2fb017bdef648268bcbcf3cf9d6c0a2b`

Step55 adds a deterministic failure layer around the fully recoverable MOCK
runtime from Steps49–54.

It does **not** wire Strategy/Risk/Planner into the venue yet. That is Step56.

## Faults

The engine supports deterministic one-shot injection of:

- delayed command response;
- lost/ambiguous submit response;
- canonical venue reject;
- user-stream disconnect;
- venue unavailable;
- stale reconciliation snapshot;
- duplicate source event;
- out-of-order source event;
- duplicate user-stream delivery;
- out-of-order user-stream delivery.

Faults change transport/venue evidence, not hidden economic truth.

## Ambiguous submit

A lost submit response is modeled as:

1. the underlying MOCK submit may be applied;
2. the caller receives `LOST_RESPONSE_AMBIGUOUS`;
3. blind retry is blocked;
4. reconciliation is required;
5. after CLEAN reconciliation the same `request_id` may be retried;
6. Step50 idempotency prevents a duplicate order mutation.

This preserves the Step47/48 rule that client-order IDs or transport errors must
not be treated as permission for blind duplicate submission.

## Disconnect / reconnect

User-stream disconnect blocks **new submits** because current venue truth cannot
be observed safely.

Cancel/modify are not blocked by the Step54 new-order reconciliation gate. They
may still fail truthfully if the venue itself is unavailable or a deterministic
rate limit is hit.

The venue may continue processing existing orders while the user stream is
disconnected. After reconnect, callers backfill from their last observed
sequence and must reconcile before new exposure.

## Venue unavailable

Venue-unavailable state:
- blocks venue commands;
- makes fresh reconciliation evidence unavailable/PENDING;
- never silently falls back to another venue or MOCK instance.

Restoring availability requires fresh reconciliation before a new submit.

## Stale snapshot

A stale injected snapshot intentionally changes only the reconciliation
sequence. Step54 therefore returns `PENDING`, not CLEAN or BLOCKED-by-fabricated
economics.

## Duplicate / out-of-order

Source duplicate:
- apply source once;
- replay identical source;
- Step53 must return `DUPLICATE_IGNORED`.

New out-of-order source:
- event time is injected below the last accepted business time;
- Step53 rejects it and marks recovery unsafe.

User-delivery duplicate/out-of-order faults alter only the observed delivery
page. They never mutate the canonical Step53 stream.

## Rate limiting

Synthetic deterministic fixed event-time windows:

- window size: 10 supplied event-time units;
- submit: 2;
- cancel: 4;
- modify: 4;
- market source: 64;
- reconciliation: 16;
- stream read: 64.

There is no wall-clock sleep or timer. Advancing to the next event-time window
recovers capacity deterministically.

`RATE_LIMITED` is marked retryable. An ambiguous lost submit response is not:
it requires reconciliation before retry.

## Deterministic chaos

Default seed:

`550055`

The optional automatic submit-fault selector uses SplitMix64 over:
- frozen seed;
- submit fault ordinal;
- operation key.

Same seed + same inputs produces the same fault decisions. The engine records
fault/outcome evidence and exposes a SHA-256 evidence fingerprint.

The gate also runs two independent campaigns with the same seed and proves:
- same user-stream fingerprint;
- same economic fingerprint;
- same chaos evidence fingerprint.

## Safety gate

New submit requires all of:
- venue available;
- user stream connected;
- no unresolved ambiguous submit;
- fresh Step54 CLEAN reconciliation;
- reconciled sequence equals current Step53 sequence;
- Step53 recovery safe;
- rate-limit capacity.

Any state advance after CLEAN invalidates the new-submit gate until another
reconciliation.

## Still deferred

Step55 does not add:
- Strategy/Risk/Planner full-system wiring;
- manual-control MOCK routing;
- dashboard simulation completion;
- real Hyperliquid signing/routing;
- smart/split routing;
- live capital.

## Next

**Step56 — Full-System Replay Runtime**
