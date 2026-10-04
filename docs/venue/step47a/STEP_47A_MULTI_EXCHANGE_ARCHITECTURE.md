# Step47A — Canonical Multi-Exchange Architecture Baseline

Contract version: `step47a-v1`
State on creation: POST Step46C; private/real routing remains disabled.

## Purpose

Freeze the venue-neutral vocabulary and architectural boundaries **before** mapping
Hyperliquid semantics in Step47B. Hyperliquid is the first real venue, not the core
architecture.

Step47A deliberately does **not** implement the Step48 `VenueAdapter` interface and does
not add HTTP/WebSocket/signing/order-routing code.

## Mandatory boundary

```text
Strategy / Portfolio-Risk / Planner
              |
       canonical intent
              v
  [Step48 Multi-Venue Adapter Contract]
       /          |            \
   MOCK      HYPERLIQUID    FUTURE_VENUE
       \          |            /
        canonical events/state
              |
 ExecutionState / Ledger / Reconciliation / Dashboard
```

The northbound core may depend on canonical identities/capabilities/errors. It must not
depend on native protocol payloads, native symbol heuristics, native reject strings or
private signing mechanisms.

## Identity model

The permanent identity chain is:

```text
market-data/source symbol
        -> canonical/internal asset
        -> explicit per-venue symbol and/or asset-id leg
```

`canonical/internal asset` is not a venue symbol. A mapping is routable only when the
configured venue leg is explicit and validated. No strip-USDT, case conversion or alias
guessing may become routing logic.

Every venue-originated canonical record must be able to retain `venueId` and environment
(MOCK/TESTNET/MAINNET/custom) together with useful native identifiers/evidence.

## Capabilities

A concrete venue adapter declares versioned capabilities. The core queries these
capabilities rather than assuming that every venue implements the same order lifecycle.
The Step47A baseline vocabulary includes metadata/rules, submit/cancel/modify, client
order ids/idempotency, GTC/IOC/post-only/reduce-only, batch actions, leverage/margin,
account snapshots, orders/fills, user streams, recovery, dead-man switch and rate-limit
introspection.

Absence of a required capability is a blocker; it is not silently emulated unless a later
step explicitly defines a safe canonical emulation.

## Error/reject model

Adapters translate native failures into a canonical `VenueErrorClass` while retaining the
native code/reason as opaque evidence. Strategy/Risk/Planner may branch on canonical
classes only. They must not branch on Hyperliquid (or any venue) strings.

`UNKNOWN` is intentionally present. Unknown semantics do not become success and must be
handled fail-closed when they affect safety/routing.

## Authentication/signing boundary

Authentication, signing, nonce/session handling and private secrets belong to the
concrete adapter/edge. They do not belong to dashboard-api, Strategy, Portfolio/Risk or
OrderPlanner.

## Routing policy v1

Multi-exchange-ready is **not** smart-order-routing.

- one venue/environment is selected explicitly by configuration/control context;
- no silent REAL -> MOCK fallback;
- no silent venue-A -> venue-B fallback;
- no split routing across venues in v1;
- adding smart/split routing later requires a deliberate contract and gate.

## Reconciliation / ledger

Reconciliation is scoped to one venue/environment truth domain. Snapshots, orders and
fills from different venues must not be merged into one truth comparison without an
explicit future aggregation contract.

Ledger and audit lineage must preserve the venue context when venue-originated events are
integrated in later steps.

Unsafe reconciliation continues to imply `NO NEW ORDERS` for the affected routing
context.

## Dashboard

The dashboard remains venue-agnostic in architecture but venue-aware as data. Existing
pages/read models should show `venueId`/environment/native evidence where useful without
creating an exchange-specific parallel dashboard.

Browser -> exchange remains forbidden.

## Future venue onboarding contract

A future `VenueAdapter_X` should primarily require:

1. semantics/capabilities study;
2. explicit Symbol Registry leg;
3. concrete adapter implementation;
4. parity/reconciliation/acceptance gates.

It must not require rewriting Strategy/Risk/Planner contracts.

## Step47A PASS condition

Step47A may close only if the gate proves:

- canonical identity/capability/error types compile independently;
- those headers contain no concrete venue/protocol names;
- current exchange gateway wording is venue-neutral;
- the machine-readable baseline declares explicit routing/no fallback;
- the portability checklist covers MOCK + first real venue + FUTURE_VENUE;
- no Step48 adapter interface or private routing implementation was smuggled into 47A.

Next after PASS: **Step47B — Hyperliquid API Surface & Venue Semantics Mapping**.
