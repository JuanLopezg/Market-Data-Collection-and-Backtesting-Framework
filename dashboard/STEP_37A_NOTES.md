# Step 37A — Multi-Exchange Symbol Registry + Drift Alarms

Status of implementation: **READY FOR LOCAL RUNTIME GATE; NOT ACCEPTED UNTIL USER OBSERVES PASS**.

## Why Step 37A exists

Step 37 private Hyperliquid TESTNET wallet/auth work is deliberately **deferred** because it depends on an external wallet/account prerequisite and is not required to continue safe engineering work.

Step 37A therefore closes a separate prerequisite that is needed before any order routing regardless of venue: explicit, versioned symbol identity across market-data sources and execution venues.

## Registry contract

The canonical dashboard artifact is:

`dashboard-api/internal/integration/symbolregistry/registry.json`

Current schema records, per internal symbol:

- exact market-data source identity (`BINANCE / SOURCE` today);
- exact market-data symbol;
- exact execution venue/environment (`HYPERLIQUID / TESTNET` today);
- exact execution symbol when mapped;
- explicit blocked state when no approved execution mapping exists;
- routing policy;
- reason for blocked classifications.

The registry is designed for future explicit legs such as another CEX/DEX or production venue. Adding a venue means adding a new explicit mapping leg; it does **not** mean introducing runtime symbol conversion heuristics.

## Non-negotiable rule

`UNREGISTERED / UNMAPPED / BLOCKED / VENUE_ABSENT / DIVERGENT => NO ORDER ROUTING`

Forbidden:

- stripping `USDT` at runtime;
- case-conversion as a mapping mechanism;
- fuzzy matching;
- choosing a “similar” token;
- silent alias substitution;
- silently dropping a target that cannot be executed.

## Drift detection

Authenticated read-only endpoint:

`GET /api/symbol-registry`

It validates:

1. registry JSON integrity and uniqueness;
2. exact consistency with the already accepted Step 35 Hyperliquid manifest;
3. coverage of the current canonical Binance/source ranking window;
4. coverage of the current strategy universe;
5. current Hyperliquid TESTNET metadata presence for mapped strategy symbols;
6. execution coverage without promoting unsupported symbols.

The endpoint may be `VALIDATED_WITH_WARNINGS` when the registry is structurally complete but current execution coverage is partial. This is expected while unsupported/venue-absent assets remain intentionally non-routable.

## Operational alarms

Step 37A integrates derived alarms into existing `Alerts & Audit`:

CRITICAL:

- `SYMBOL_REGISTRY_BLOCKED`
- `SYMBOL_REGISTRY_DIVERGENCE`
- `SYMBOL_UNREGISTERED_MARKET_DATA`
- `SYMBOL_UNREGISTERED_STRATEGY`

WARN:

- `SYMBOL_VENUE_ABSENT`
- `SYMBOL_COVERAGE_PARTIAL`

The current alert page is still a derived read model, not an append-only alert store. Durable alert history/acknowledgement remains a later product milestone.

## Safety boundary

Step 37A requires:

- no wallet;
- no API wallet;
- no private key;
- no signing;
- no `/exchange` call;
- no submit/cancel;
- no real capital.

Private auth remains `DEFERRED` and order routing remains `DISABLED`.

## Acceptance

Run:

`./scripts/step37a-symbol-registry.sh`

Expected final line:

`STEP 37A: PASS — MULTI-EXCHANGE SYMBOL REGISTRY + ALARMS VALIDATED`

A new or changed Binance/source symbol that is not explicitly registered must fail this gate. A mapping disagreement with Step 35 must also fail the gate.

## v0.37A.3 — live ranking drift classification

The canonical ranking later introduced nine symbols absent from the initial registry snapshot: `AEROUSDT`, `ARKUSDT`, `BRUSDT`, `BTWUSDT`, `LITUSDT`, `LYNUSDT`, `MUBARAKUSDT`, `PHAUSDT`, and `RAREUSDT`. The live gate correctly failed closed. They are now explicit Binance/source entries and explicit `BLOCKED_EXPLICIT` Hyperliquid TESTNET classifications pending manual mapping review. No execution alias is guessed.
