# Step53 — Mock Snapshot / User Stream / Recovery

**Entering state:** Step52 PASS/CLOSED.  
**Bound Step52 accounting fingerprint:** `4fa2a7cc0e8c43f75f7fdcab2cd5b305755c48426ac3f13b4c10ed765dfac746`

Step53 makes the MOCK execution/account state restart-recoverable and exposes a
deterministic canonical user stream.

## Durable recovery model

The v1 checkpoint is a **deterministic replay checkpoint**:

- `checkpoint_v1.bin` contains every deterministic source record through the
  checkpoint, the checkpoint stream sequence and the Step52 economic fingerprint.
- `incremental_v1.bin` contains source records appended after the checkpoint.
- every incremental record is length-framed and fsynced before it is applied.
- a checkpoint is written to a temporary file, fsynced, renamed and the parent
  directory is fsynced before the incremental journal is truncated.

The checkpoint is deliberately self-contained rather than depending on private
memory layout from Steps50–52. This preserves the frozen earlier-step headers.

On restart the runtime:
1. replays the checkpoint records;
2. verifies the exact checkpoint user-stream sequence;
3. verifies the exact Step52 economic fingerprint;
4. replays post-checkpoint incrementals.

Any corrupt/truncated record or replay divergence fails closed.

## Why source replay is used

Steps50–52 are hash-frozen. Their private request cache, matcher fill counters and
account dedup state cannot be injected safely without mutating those historical
contracts.

Replaying the durable deterministic source recipe reconstructs:
- Step50 request-id/client-id/order lifecycle state;
- Step51 latency/fill-sequence state;
- Step52 fill dedup/accounting/positions/marks/leverage state.

This is a MOCK recovery implementation, not a claim that a real venue should
recover by replaying local source history.

## User stream

Every observable canonical event receives a deterministic monotonically
increasing sequence beginning at 1.

The stream includes the events produced by Steps50–52:
- OperationResult;
- OrderUpdate;
- Fill;
- AccountingEvent.

Snapshot objects themselves are read surfaces and are not inserted back into the
incremental stream.

## Snapshot + incremental merge

A client takes a state snapshot at sequence `S`:

- canonical `AccountSnapshot`;
- canonical `OpenOrdersSnapshot`;
- canonical `FillBatch`;
- exact Step52 economic fingerprint;
- deterministic user-stream fingerprint.

It then asks for:

`streamAfter(S, limit)`

and applies only events with sequence `> S`.

Backfill is bounded to 256 stream events per page. Cursor meaning is always the
last delivered sequence.

## Duplicate and conflict rules

Commands:
- Step50 request-id semantics are reconstructed by replay.
- a known request-id retry may arrive with its original event time.

Non-command source events:
- market observation identity = canonical asset + event time;
- mark identity = canonical asset + event time;
- leverage identity = canonical asset + event time;
- rebate/funding identity = explicit event ID.

Same identity + same payload:
`DUPLICATE_IGNORED`.

Same identity + different payload:
`IDENTITY_CONFLICT_UNSAFE`.

## Out-of-order policy

A genuinely new source event with event time older than the last accepted
business event is rejected and marks recovery unsafe.

Known exact duplicates and known request-id retries may arrive late because they
do not create new economics.

While recovery is unsafe, a new checkpoint is refused.

## Time semantics

Persistence uses technical file/fsync operations, but economic timestamps are
still only the supplied business/event timestamps.

No wall-clock timestamp participates in:
- matching;
- fills;
- PnL;
- funding;
- fees;
- stream order;
- recovery fingerprints.

## Still deferred

Step53 does **not** implement:
- local-vs-venue reconciliation;
- ledger parity;
- real venue account truth;
- private Hyperliquid authentication/signing/routing;
- smart-order-routing;
- live capital.

## Next

**Step54 — Reconciliation + Ledger Parity**
