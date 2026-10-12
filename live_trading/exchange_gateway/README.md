# Exchange gateway and real venues

The gateway receives `SubmitOrder`, `CancelOrder` and exchange-snapshot requests from
the execution services and returns order updates, fills and snapshots. It has three
modes:

| Mode | Behind the gateway |
| --- | --- |
| `backend` | simulated exchange over a private NATS stream (PAPER, replay) |
| `venue` | a real exchange through `venue/LiveVenueAdapter` (currently Kraken Futures) |
| `hyperliquid-dry-run` | plan inspection only; no order path |

## Two levels: one gateway, one adapter per kind of exchange

```text
exchange_gateway_main.cpp          common gateway: NATS commands in, events out, time checks
    -> ExchangeGatewayAdapter      common interface (lib/src/exchange/exchange_gateway_adapter.h)
         BackendGateway            simulated exchange (lib/src/exchange/adapters/backend_gateway.*)
                                   -> simulated_exchange_service process (matching, account, outbox)
         LiveVenueAdapter          every real REST exchange (venue/)
              -> VenueProtocol     one file per exchange: KrakenFuturesProtocol today
```

The simulated exchange is deliberately not a `VenueProtocol`. It is a separate durable
process reached over a private NATS stream (state and outbox committed together), not
an HTTP API with signing, catalogues and rate limits. Forcing it behind a fake HTTP
protocol would change its accepted crash/recovery contract and replay economics for no
benefit. Choosing simulated or Kraken is one gateway option: `--mode backend` or
`--mode venue --venue kraken-futures`.

## One adapter, one protocol per real exchange

```text
exchange_gateway_main.cpp
    -> LiveVenueAdapter (venue/live_venue_adapter.*)     common to every exchange
         uses VenueJournal (venue/venue_journal.*)       durable orders/fills (SQLite)
         uses HttpTransport (venue/http_transport.*)     libcurl; GetOnlyTransport guard
         uses VenueProtocol (venue/venue_protocol.h)     the exchange-specific part
              KrakenFuturesProtocol (venue/kraken_futures_protocol.*)
```

`LiveVenueAdapter` owns everything that does not depend on the exchange: the
submission gate, journaling before acknowledgement, holding orders until their
`active_from` business day, sending, ambiguous-outcome handling, cancels, fill
polling and de-duplication, local fill ids, unit conversion back to source symbols,
snapshots, rate budget and event publication.

`VenueProtocol` is what a new exchange replaces: authentication/signing, endpoint
URLs, market catalogue and symbol naming, quantity steps, request formats, response
parsing, status and error mapping, client order id format and request costs. The
adapter never branches on the venue name. To add an exchange, implement one
`VenueProtocol`, add recorded responses to `venue/venue_adapter_test.cpp`, and select
it in `createVenueAdapter()`.

## Safety rules (enforced in code)

- `--order-submission` defaults to `disabled`. Then every order is rejected locally
  ("order submission disabled") and the transport is wrapped in `GetOnlyTransport`:
  any POST throws before leaving the process.
- With submission enabled the key must have trade permission; keys with transfer or
  withdrawal permission are always refused.
- An order is journaled (`PREPARED`) before the NATS command is acknowledged and
  marked `SENDING` (committed) before the HTTP request leaves.
- No answer, a timeout or a 5xx makes the order `UNKNOWN`. It is resolved only from
  venue evidence (open orders, fills) and never resent. After two minutes without
  evidence the adapter raises an ALERT for manual resolution. A crash during sending
  becomes `UNKNOWN` on restart.
- Errors documented as "not processed" (`apiLimitExceeded`, nonce errors, HTTP 429)
  return the order to `PREPARED`, so it is sent again safely.
- Fills are de-duplicated by venue fill id; each gets a stable local `FillID` in the
  same transaction that updates the order. Event message ids are deterministic, so a
  re-publication after a crash is a duplicate, not a new event.
- Fills that no order explains (manual trades, liquidations) or that predate their
  order (a reused client id) are recorded and reported, never attributed.
- Markets the venue does not list, does not trade, or where the rounded quantity is
  below the venue step are rejected with `MARKET_UNAVAILABLE`, `MARKET_NOT_TRADING`
  or `BELOW_MINIMUM_QUANTITY`. No other product or venue is substituted.
- Quantities are rounded **down** to the venue step, so never more than requested.
  The fill quantities, not the requested quantity, drive accounting.

Order states in the journal: `PREPARED`, `SENDING`, `OPEN`, `UNKNOWN`, `FILLED`,
`CANCELLED` (also partial fills whose remainder ended), `REJECTED`. Only `OPEN`
(Accepted), `FILLED`, `CANCELLED` and `REJECTED` are reported to execution.

## Kraken Futures mapping

Checked against docs.kraken.com (Futures REST reference, authentication guide and
rate-limit guide) on 2026-10-11. Private calls use the production host
`https://futures.kraken.com`; there is no supported demo (the user's decision is to
test with small, individually authorized real-money orders).

| Need | Kraken endpoint | Notes |
| --- | --- | --- |
| Key permissions | `GET /api/auth/v1/api-keys/v3/check` | refuse transfer/withdraw |
| Markets | `GET /derivatives/api/v3/instruments` (public) | `PF_` flexible perpetuals, quote USD, `tradfi` excluded; `XBT` = BTC; `contractSize` must be 1; step = 10^-`contractValueTradePrecision` |
| Submit | `POST /derivatives/api/v3/sendorder` | `orderType=mkt` (IOC with 1% price protection), `symbol`, `side`, `size`, `cliOrdId` |
| Cancel | `POST /derivatives/api/v3/cancelorder` | by `cliOrdId`; `cancelled` / `filled` / `notFound` |
| Open orders | `GET /derivatives/api/v3/openorders` | evidence for `UNKNOWN` orders |
| Fills | `GET /derivatives/api/v3/fills` | last 100, newest first; includes `cliOrdId`; **no fee field** |
| Snapshot | `GET accounts`, `openpositions`, `openorders` | cash = USD quantity of the Multi-M (`flex`) wallet |
| Dead man's switch | `POST /derivatives/api/v3/cancelallordersafter` | off by default; path/parameter UNCONFIRMED |

Signing: `Authent = Base64(HMAC-SHA512(Base64Decode(secret), SHA256(postData + nonce + path)))`
with `path` without the `/derivatives` prefix. POST requests send the form body as
`postData` and a millisecond `Nonce` header; GETs send no nonce. The C++ signature
equals the Python reference (offline test) and was accepted by Kraken for all read
endpoints (read-only probe below).

Client order ids: `algo-<account namespace>-<order id>` (Kraken allows 100 characters).
Change the namespace whenever the execution state is reset, so old ids cannot be reused.

Scaled Binance contracts (`1000PEPEUSDT`, `1000000MOGUSDT`, ...) are converted to
underlying units (x1000, x1,000,000) for Kraken and back for fills; quantity x price
stays the same USD notional. The alias list is shared with `kraken_shadow.py`.
Pending (docs/ROADMAP.md 7b): the list is maintained by hand. A new Binance `1000X`
listing missing from it maps to asset `1000X`, which Kraken does not list, so it is
skipped rather than mis-sized; the risk is a wrong entry, or a venue that itself lists a
scaled name, giving orders 1000x too small or too large. Derive or check the scale from
instrument data, skip explicitly on doubt, guard the USD notional before submission and
test end to end before real orders on scaled assets.

Rate budget: 500 units per 10 s; sendorder/cancelorder 10, reads 2, dead man's switch 25.

### Open points to confirm with the first authorized real order

- Permission value of a trading key (assumed `general = FULL_ACCESS`).
- `sendorder` response for `mkt` orders: status (`placed` vs `filled`) and events.
- Whether `clientOrderIdAlreadyExist` applies only to open orders (treated as `UNKNOWN`).
- `cancelallordersafter` path, parameter and response (keep disabled until then).
- Commissions: fills carry none, so fills are published with commission 0 for now;
  actual fees and funding need the account log (roadmap step 9).
- Snapshot cash is the USD wallet balance; BTC collateral valuation and margin
  belong to roadmap step 8 and the account monitor, not to this snapshot.

## Run and test

```bash
# Offline: recorded responses only (72 checks).
build/live_trading/exchange_gateway/src/algotrading_venue_adapter_test

# Real, read-only: GET only through GetOnlyTransport; prints counts, never credentials.
build/live_trading/exchange_gateway/src/algotrading_venue_adapter_test --live-read-only <credentials file>

# Gateway in venue mode (read-only unless --order-submission enabled is given):
algotrading_exchange_gateway --mode venue --venue kraken-futures \
  --credentials-file /secrets/kraken.env --venue-journal /data/venue/kraken.sqlite \
  --account-namespace k20261011 --nats-url nats://nats:4222 --stream ... --control-stream ...
```

The credentials file contains exactly `KRAKEN_API_KEY=...` and `KRAKEN_API_SECRET=...`.
Never print it. Enabling `--order-submission enabled` and any real order require the
user's explicit authorization for that specific test.
