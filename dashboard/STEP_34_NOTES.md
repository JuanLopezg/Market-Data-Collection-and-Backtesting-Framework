# Step 34 — Public Venue Connectivity / Exchange Metadata

Status in this archive: implementation complete; final live TESTNET network acceptance must be run from the user's WSL/Docker environment.

## Goal

Prove that the dashboard can reach the intended Hyperliquid TESTNET public API and read enough public market metadata to support the next symbol-mapping gate, without introducing any private credential, signature, account query or trading action.

## External contract used

Pinned endpoint:

`https://api.hyperliquid-testnet.xyz/info`

Fixed public queries only:

- `POST {"type":"meta"}` — perpetual universe metadata;
- `POST {"type":"allMids"}` — public mids.

The official Hyperliquid Info API documents the `POST /info` model, `allMids`, and perpetual `meta` request types. Testnet uses the corresponding `api.hyperliquid-testnet.xyz` host.

## Implementation

New Go adapter:

`dashboard-api/internal/integration/hyperliquid/public_client.go`

Properties:

- HTTP POST only;
- fixed request bodies (`meta`, `allMids`);
- bounded response reads;
- bounded timeout;
- redirects disabled;
- no Authorization header;
- no wallet/user address;
- no `/exchange` action endpoint;
- no secret/signing material.

New authenticated dashboard endpoint:

`GET /api/venue-public`

It returns a bounded summary only: public reachability, metadata/mid availability, counts, overlap, a short symbol sample, latency and safety-state fields.

## UI

Infrastructure now includes:

`Step 34 · Public Venue Connectivity / Metadata`

This panel is intentionally distinct from the existing `Exchange Connectivity` block. Public REST metadata reachability must never be presented as private/execution-gateway connectivity.

## Fail-closed boundaries preserved

- `privateAuth = DISABLED`
- `orderRouting = DISABLED`
- `symbolMapping = NOT_CONFIGURED`
- `exchangeFilters = NOT_APPLIED`
- `secretsUsed = false`
- `capitalUsed = false`
- Manual Control route remains disabled.
- `Infrastructure.exchange.connected` remains false/UNKNOWN until an execution-runtime-owned connectivity contract exists.

## Configuration

Real local compose pins:

- `DASHBOARD_HYPERLIQUID_PUBLIC_INFO_URL=https://api.hyperliquid-testnet.xyz/info`
- `DASHBOARD_HYPERLIQUID_PUBLIC_TIMEOUT=3s`

The API validates the URL fail-closed to the exact HTTPS TESTNET `/info` endpoint. This prevents configuration from turning the dashboard public probe into an arbitrary URL fetcher.

## Acceptance

After starting the normal local real stack, run:

`./scripts/step34-public-venue.sh`

A PASS requires:

- Step 33 remains valid;
- source/build safety gates remain valid;
- public TESTNET `meta` and `allMids` succeed;
- returned universe and mids are non-empty and overlap;
- no private/auth/order capability appears;
- execution connectivity remains fail-closed;
- Manual Control route remains disabled.

A Step 34 PASS authorizes only progression to Step 35 symbol-mapping validation. It is not trading readiness.
