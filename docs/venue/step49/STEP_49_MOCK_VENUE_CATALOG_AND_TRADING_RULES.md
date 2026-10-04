# Step49 — Mock Venue Catalog & Trading Rules

Entering state: Step48 PASS/CLOSED.

Step49 creates the first concrete venue-specific catalog/rule leg behind the
canonical multi-venue contract. It does **not** yet implement order admission,
order lifecycle, matching/fills, accounting, a MockExchangeAdapter, or any
private real-venue routing.

## Explicit catalog

The frozen source universe is `2026-09-27-step37a.5` with 175 internal assets.

Every MOCK market is explicitly listed as:

`canonical/internal asset -> MOCK venue symbol -> MOCK native asset id -> rule profile`

The v1 MOCK symbol intentionally equals the existing internal identifier, but
that equality is data in `catalog_v1.json`; runtime code never derives it.

No case conversion, suffix stripping, fuzzy matching or alias guessing exists.
A later source asset is non-routable to MOCK until a new explicit catalog version
adds it.

## Synthetic rule profile

All 175 entries use `MOCK_PERP_DEFAULT_V1`:

- product: PERPETUAL
- synthetic settlement asset: USD
- price increment: 0.00000001
- size increment: 0.00000001
- minimum size: 0.00000001
- minimum notional: 10.00
- max leverage metadata: 20
- margin metadata: CROSS + ISOLATED
- TIF metadata: GTC + IOC
- post-only: yes
- reduce-only: yes
- modify: yes
- client order ID: yes
- native MOCK idempotent submit: yes
- precision policy: REJECT_NONCONFORMING

These are deliberately synthetic deterministic rules. They are **not** described
as Hyperliquid parity.

## Exact decimal metadata

Increments/minimums are frozen as base-10 strings. Step49 does not use binary
floating point to define tick/lot rules. Step50 will implement exact admission
arithmetic against these values.

## MOCK is not a Hyperliquid listing clone

The MOCK catalog explicitly contains all 175 frozen source/internal assets,
including assets whose Hyperliquid TESTNET leg is blocked. This is intentional:
full-system replay must be able to exercise the pipeline independently of one
real venue's listing set.

Hyperliquid availability remains a separate venue leg and is revalidated later.

## Fingerprint

`manifest_v1.json` freezes catalog, rules and source-universe SHA-256 values plus
one combined MOCK venue fingerprint. Later deterministic replay evidence can bind
a run to this exact catalog/rules version.

## Next

Step50 — Mock Order Admission & Lifecycle.

Step50 may consume this catalog/rules fingerprint, but must not silently mutate
Step49's frozen files.
