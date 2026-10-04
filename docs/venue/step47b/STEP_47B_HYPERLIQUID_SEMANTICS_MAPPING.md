# Step47B — Hyperliquid API Surface & Venue Semantics Mapping

**State entering this step:** Step47A PASS/CLOSED.  
**Architecture rule:** Hyperliquid is the first real venue, not the architecture.  
**This step:** documentation + semantic mapping only. No private auth, signing,
wallet creation, order submission, capital movement or runtime adapter implementation.

## What was reviewed

The official Hyperliquid GitBook API material was reviewed on 2026-09-27:
- API root and TESTNET/MAINNET base endpoints;
- `/info`, including perpetual and spot sub-sections;
- `/exchange`;
- asset IDs;
- tick/lot/price precision;
- error responses;
- WebSocket connection, subscriptions, post requests and heartbeats;
- rate/user/open-order limits;
- signing;
- nonces and API/agent wallets;
- margining.

The exact official URLs are frozen in `HYPERLIQUID_SOURCE_MANIFEST.md`.

## Mapping rule

Every reviewed Hyperliquid semantic is classified as one of:

- `CANONICAL_MATCH` — already represented by Step47A without venue leakage.
- `CANONICAL_WITH_VENUE_EXTENSION` — canonical concept exists but wire/rule
  details remain Hyperliquid-owned.
- `HYPERLIQUID_ONLY` — optional venue feature that is not promoted to core.
- `UNSUPPORTED_BY_CANONICAL_MODEL` — Step47A is insufficient and Step47C must
  resolve/freeze a venue-neutral contract decision.

Every feature is also tagged with one or more roadmap scope classes:
`REQUIRED_FOR_CURRENT_STRATEGY`, `REQUIRED_FOR_REAL_RECONCILIATION`,
`MOCK_REQUIRED`, `DEFERRED_FEATURE`, `OUT_OF_SCOPE`.

## Confirmed Step47A capability coverage

Hyperliquid documentation confirms direct support for the Step47A capabilities:
market metadata/rules, submit/cancel/modify, client IDs, GTC/IOC/post-only,
reduce-only, batching, leverage/margin state, account snapshot, open/historical
orders, fills, user streams, snapshot/backfill, scheduled cancel/dead-man switch
and user rate-limit introspection.

**One important exception:** `IDEMPOTENT_SUBMIT` is **not confirmed** merely by
the existence of `cloid`. The official pages reviewed document client order IDs
but do not state that same-cloid re-submission is an idempotency guarantee.
Step47C must define the system's canonical idempotency policy explicitly.

## Important venue semantics

### Identity / symbols
Hyperliquid action `asset` values are native integers. Default perpetuals,
builder-deployed/HIP-3 perps, spot and outcomes have different ID schemes.
MAINNET and TESTNET IDs may differ. UI names can also be remapped relative to
HyperCore/L1 names.

**Project consequence:** retain the permanent fail-closed chain:

`source symbol -> canonical/internal asset -> explicit per-venue/environment leg`

No symbol stripping, case guessing or UI-name heuristics.

### Orders
Documented order semantics include:
- batched order actions;
- GTC / IOC / ALO(post-only);
- reduce-only;
- optional `cloid`;
- cancel by oid and by cloid;
- modify and batchModify;
- trigger TP/SL semantics and TP/SL grouping;
- scheduleCancel dead-man switch.

Trigger/TP-SL and grouping are not currently represented in Step47A and are
recorded as Step47C decisions rather than silently added to the core.

### Account / reconciliation
Relevant read surfaces include:
- `clearinghouseState`;
- open orders;
- `orderStatus`;
- historical orders;
- fills / fills by time;
- WebSocket order updates, user fills and account/open-order state.

Agent/API wallets sign, but account state queries use the actual master/subaccount
address. This distinction stays inside the private Hyperliquid adapter boundary.

### Stream recovery
The docs require handling server disconnects. Snapshot acknowledgements and
`isSnapshot` semantics are available for streaming user data; corresponding
`/info` calls can recover missed state.

This maps directly to the project rule:
snapshot + incremental merge + reconnect/backfill + idempotency + reconciliation.

### Rate limits
Hyperliquid has both:
- per-IP REST/WebSocket limits; and
- address-based action limits, plus open-order limits.

Exact values are venue-native configuration/evidence and must be revalidated at
Step62 before private integration. The Mock should reproduce the relevant
contractual failure modes, not hard-code a claim that they are universal.

### Errors
Hyperliquid has detailed order/cancel reject reasons and a special batch behavior:
most batched errors are item-shaped, while some pre-validation failures reject
the full payload with one error.

The native reason is always preserved. Step47C must freeze a canonical
operation/per-item result envelope and decide whether venue/risk-limit errors
deserve a stronger canonical class than `UNKNOWN`.

## Product-scope warning

Reviewing spot, outcomes, HIP-3, transfers, staking, vaults and TWAP does **not**
mean they enter v1 execution scope.

Step47C must freeze v1 explicitly. Step47B keeps:
- capital transfers/withdrawals/staking/vault operations: `OUT_OF_SCOPE`;
- spot/outcomes/TWAP/HIP-3 execution: `DEFERRED_FEATURE` unless deliberately
  promoted later.

## Artifacts

- `hyperliquid_semantics_map.json` — authoritative structured mapping.
- `hyperliquid_scope_matrix.csv` — human/tabular mapping.
- `HYPERLIQUID_SOURCE_MANIFEST.md` — official documentation evidence list.
- `KNOWN_GAPS_FOR_STEP47C.md` — unresolved canonical decisions.
- `SHA256SUMS` — frozen artifact hashes.

## Step47B close condition

Step47B may close when the gate proves:
1. Step47A remains present and unchanged as baseline.
2. all Step47A capabilities have a Hyperliquid assessment;
3. every mapping points to an official source key;
4. unsupported/gap semantics are explicit rather than silently normalized;
5. no secret/private key/signing/runtime order path was introduced;
6. Step47C is the next safe step.

Step47B does **not** mean private Hyperliquid integration is ready.
