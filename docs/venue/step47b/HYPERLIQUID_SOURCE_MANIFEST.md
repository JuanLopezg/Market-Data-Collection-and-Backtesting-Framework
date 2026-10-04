# Step47B — Official Hyperliquid Source Manifest

Retrieved/reviewed: 2026-09-27

Only official Hyperliquid GitBook documentation is treated as authoritative for this Step47B artifact.
Step47B is a documentation/semantic mapping step. It does not call private APIs, sign requests or place orders.

## API_ROOT — API
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api

Used for:
- mainnet and testnet REST base URLs
- official API section and SDK references

## INFO — Info endpoint
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/info-endpoint

Used for:
- open orders, fills, fills-by-time, orderStatus, historicalOrders, userRateLimit
- actual account address must be used instead of agent wallet address
- perp/spot coin naming, remapping warning, time-range pagination

## INFO_PERPS — Info endpoint — Perpetuals
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/info-endpoint/perpetuals

Used for:
- perpDexs, meta, metaAndAssetCtxs, clearinghouseState, activeAssetData
- perp dex dimension and margin/account state

## INFO_SPOT — Info endpoint — Spot
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/info-endpoint/spot

Used for:
- spotMeta, spotMetaAndAssetCtxs, spotClearinghouseState, token IDs
- outcomeMeta is testnet-only

## EXCHANGE — Exchange endpoint
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/exchange-endpoint

Used for:
- signed order/cancel/cancelByCloid/modify/batchModify/scheduleCancel actions
- GTC/IOC/ALO, reduceOnly, cloid, trigger orders, leverage, isolated margin
- vault/subaccount signing context, approveAgent, TWAP and other actions

## ASSET_IDS — Asset IDs
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/asset-ids

Used for:
- perp, HIP-3 perp, spot and outcome asset ID encoding
- mainnet/testnet asset IDs differ

## TICK_LOT — Tick and lot size
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/tick-and-lot-size

Used for:
- price significant-figure and decimal constraints
- size precision from szDecimals

## ERRORS — Error responses
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/error-responses

Used for:
- batched order/cancel errors, minimum trade notional, margin/reduce-only/post-only/IOC errors
- payload-wide pre-validation error behavior

## WS — Websocket
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket

Used for:
- mainnet/testnet WebSocket URLs
- disconnect/reconnect requirement and snapshot/info recovery guidance

## WS_SUBS — Websocket — Subscriptions
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket/subscriptions

Used for:
- orderUpdates, userFills, userEvents, clearinghouseState, openOrders and market feeds
- snapshot ack and isSnapshot semantics

## WS_POST — Websocket — Post requests
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket/post-requests

Used for:
- info and signed action requests can also be posted over WebSocket
- request id correlation

## WS_HEARTBEAT — Websocket — Timeouts and heartbeats
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/websocket/timeouts-and-heartbeats

Used for:
- 60 second inactivity close behavior
- ping/pong heartbeat format

## RATE_LIMITS — Rate limits and user limits
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/rate-limits-and-user-limits

Used for:
- REST weights, WebSocket connection/subscription/message limits
- address-based action limits and open-order limits

## SIGNING — Signing
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/signing

Used for:
- two signing schemes and msgpack/signature pitfalls
- signing remains a venue-native adapter concern

## NONCES — Nonces and API wallets
https://hyperliquid.gitbook.io/hyperliquid-docs/for-developers/api/nonces-and-api-wallets

Used for:
- API/agent wallets sign on behalf of account/subaccounts
- nonce window/tracker semantics and signer-specific nonce state
- do not use agent wallet address for account info queries

## MARGINING — Margining
https://hyperliquid.gitbook.io/hyperliquid-docs/trading/margining

Used for:
- cross, isolated and isolated-only margin concepts
- leverage and maintenance/initial margin semantics
