# Step47B — Known Gaps to Resolve in Step47C

Step47B intentionally does **not** change the Step47A canonical contract. It maps
current Hyperliquid semantics against that baseline and records where the baseline
is incomplete. These are inputs to Step47C, not hidden assumptions.

## GAP-001 — Canonical order lifecycle vocabulary
Hyperliquid exposes `open`, `filled`, `canceled`, `triggered`, `rejected` plus a
set of venue-driven cancel/reject terminal states. Step47A has capabilities but
does not define canonical order lifecycle/status semantics.

Step47C must define a minimal canonical lifecycle that allows:
- submit accepted / resting / partially-filled / filled;
- cancel requested / canceled;
- rejected;
- venue-driven terminal cancel;
- unknown/unreconciled state;
while preserving the native Hyperliquid status/reason.

## GAP-002 — Idempotent submit is not proven by `cloid`
Hyperliquid documents an optional 128-bit client order ID (`cloid`), but the
reviewed official pages do not state that repeated submission with the same
`cloid` is an idempotent-submit guarantee.

Step47C must not equate CLIENT_ORDER_ID with IDEMPOTENT_SUBMIT. It must define
the canonical/local idempotency strategy required by Step48/Mock.

## GAP-003 — Trigger / TP-SL capability
Hyperliquid supports trigger order fields and TP/SL grouping. Step47A has no
canonical trigger-order capability. Current PureRSI v1 does not require this to
close the first execution subset, so it may remain deferred, but the omission
must be explicit.

## GAP-004 — Fee and funding accounting vocabulary
The mock roadmap requires deterministic fees and funding where applicable.
Hyperliquid exposes fee information and user funding streams, but Step47A has no
fee/funding capability or canonical accounting vocabulary. Step47C must define
the minimum canonical fields needed for Step52 without making Hyperliquid's
payload universal.

## GAP-005 — Canonical product / market identity
Hyperliquid distinguishes default perps, builder-deployed perps (DEX dimension),
spot and outcomes. `VenueAssetIdentity` can hold a native symbol/id but Step47A
does not explicitly distinguish product/market family. Step47C must freeze the
v1 product identity needed to prevent canonical `BTC` from ambiguously meaning
spot vs perp.

## GAP-006 — Batch operation result envelope
Most batched errors are per-item, but some deterministic pre-validation failures
reject the whole payload with one error. Step47A has no operation-level vs
per-item result envelope. Step47C must define this before Step48.

## GAP-007 — Venue/risk-limit reject taxonomy
Hyperliquid distinguishes open-interest-cap, oracle and max-position rejections.
Step47A can preserve them as `native_reason`, but mapping all of them to UNKNOWN
would make observability and deterministic Mock rejection tests unnecessarily
weak. Step47C must decide whether to add a canonical VENUE_LIMIT/RISK_LIMIT class
or intentionally keep specific cases native-only.

## GAP-008 — Native fill identity
The WebSocket docs indicate that `tid` alone should not be assumed globally
unique; a composite such as block time + coin + tid can be needed. Step47C must
freeze the rule that `native_fill_id` is an opaque stable identifier constructed
by the adapter, not a bare Hyperliquid `tid`.

## GAP-009 — V1 product scope
Step47C must explicitly freeze whether the first Hyperliquid private adapter
supports:
- default perpetuals only (recommended minimum based on current project scope),
- HIP-3 perps,
- spot,
- outcomes.

Step47B does not silently expand scope to spot/outcomes.

## Not gaps / deliberate venue extensions
The following remain adapter-owned unless Step47C decides otherwise:
- nonce tracker/window;
- signing scheme and msgpack details;
- REST vs WebSocket post transport;
- `expiresAfter`;
- subaccount/vault account-selection mechanics;
- Hyperliquid-specific request weights and connection limits;
- TWAP and capital-transfer/staking/vault actions.

## Step47C gate condition
No gap may be resolved by adding Hyperliquid strings, payload structs or protocol
branching inside Strategy / PortfolioRisk / OrderPlanner.
