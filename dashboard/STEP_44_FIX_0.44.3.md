# Step 44 Fix 0.44.3 — transient readiness-source classification without weakening fail-closed semantics

## Failure observed

The Step 44 global readiness endpoint correctly remained `BLOCKED`, but one request captured several short-lived read/transport failures at once:

- canonical market-data SQLite timeout;
- public venue-rules SQLite timeout;
- symbol-registry SQLite timeout;
- manual-control PostgreSQL timeout;
- NATS / JetStream read path temporarily reported `CRITICAL`.

The first three were already classified as bounded retryable blockers. The manual-control timeout was not, and the readiness snapshot discarded the NATS diagnostic detail, so a transient NATS transport failure could not be distinguished from a semantic/configuration failure. The Step 44 gate therefore stopped immediately instead of retrying the entire fail-closed snapshot.

## Fix

- Preserve PostgreSQL `persistenceState` and NATS `ackHealthLabel` in the Step 44 infrastructure snapshot.
- Surface the concrete infrastructure diagnostic in the readiness requirement instead of the generic `Read path state is CRITICAL` message.
- Allow `manual-route`, `postgres`, and `nats` to be classified as retryable **only when** their detail contains a known bounded transport/read marker.
- Recognized transient markers include timeouts, connection reset/refused, temporary unavailability, SQLite busy/locked, and equivalent network-unreachable conditions.
- Keep configuration/semantic failures non-retryable. Example: `NATS source is not configured` remains an immediate hard blocker.
- The endpoint itself remains fail-closed on every failed read. Retryability is metadata for the gate; it never turns a blocked snapshot into PASS.

## Regression coverage

Added tests proving:

- manual-control PostgreSQL timeout => `BLOCKED` + retryable + manual routing remains `UNKNOWN`;
- transient NATS transport failure => `BLOCKED` + retryable;
- NATS configuration failure => `BLOCKED` + non-retryable;
- the exact five-blocker pattern observed in Step 44 remains blocked but all five are retryable, allowing the shell gate to take a fresh bounded snapshot.

## Safety

No wallet, signing, exchange command, order submission/cancellation, trading mutation, or routing enablement was added. Persistent failures still exhaust the bounded gate retries and fail. Semantic contradictions still fail immediately.
