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
- Source symbols reach the venue only through the explicit symbol map (below). A symbol
  without an entry is rejected with `MARKET_UNAVAILABLE`; names are never guessed.
- Before an order is journaled, its USD value at the venue (venue quantity x current
  venue mark price) must be within 1.5x of the planned value (source quantity x the
  planner's `reference_price`). Otherwise it is rejected with `NOTIONAL_MISMATCH` and an
  ALERT; an order without a planning price is rejected with `NO_REFERENCE_PRICE`. A
  wrong unit scale (1000x) can therefore never reach the venue.

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
| Mark prices | `GET /derivatives/api/v3/tickers` (public) | `markPrice` per `PF_` symbol for the USD value check; cached 60 s |
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

### Symbol map (Binance -> Kraken)

`config/venues/kraken_futures/symbol_map.json` is the one table that turns strategy
symbols into Kraken contracts. The gateway (`venue/symbol_map.*`, `--symbol-map`),
`live_trading/kraken_shadow.py` and the dashboard (Symbol Mapping page) all read it:

```json
{"source": "1000PEPEUSDT", "asset": "PEPE", "venue_symbol": "PF_PEPEUSD", "venue_units_per_source_unit": 1000}
```

venue quantity = source quantity x scale; venue price = source price / scale; the USD
value is unchanged. Fills and snapshot positions are converted back to source units.

The table is built, never edited by hand, with
`python3 tools/kraken_symbol_map.py --output config/venues/kraken_futures/symbol_map.json`.
It maps a Binance USDT perpetual only when Kraken lists a tradeable `PF_` contract with
`contractSize` 1 whose name is the Binance base, or the base without an explicit
`1000`/`1000000`/`1M` prefix, **and** the Binance/Kraken mark-price ratio is within 5% of
that scale. Other digits are never stripped (`1INCH`). Review its printed rejections and
the git diff before committing. Three checks keep it honest afterwards:

1. the gateway's USD value check on every order (above);
2. the dashboard compares Binance closes with Kraken mark prices for the strategy universe
   and top-50 ranking every minute: a wrong scale is a CRITICAL alert, a strategy asset
   Kraken cannot trade a WARN (CRITICAL while its signal is active), a top-50 asset that
   Kraken lists but the table lacks a WARN (rebuild the table), and a top-50 asset Kraken
   does not list an INFO;
3. at market reload the gateway logs mapped contracts Kraken no longer lists.

Pending (docs/ROADMAP.md 7b): MOCK/replay quantities in their own units are exact only up
to about 10 million units (double precision).

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
# Offline: recorded responses only (85 checks).
build/live_trading/exchange_gateway/src/algotrading_venue_adapter_test

# Real, read-only: GET only through GetOnlyTransport; prints counts, never credentials.
build/live_trading/exchange_gateway/src/algotrading_venue_adapter_test --live-read-only <credentials file> \
  config/venues/kraken_futures/symbol_map.json

# Gateway in venue mode (read-only unless --order-submission enabled is given):
algotrading_exchange_gateway --mode venue --venue kraken-futures \
  --credentials-file /secrets/kraken.env --symbol-map /config/symbol_map.json \
  --venue-journal /data/venue/kraken.sqlite \
  --account-namespace k20261011 --nats-url nats://nats:4222 --stream ... --control-stream ...
```

The credentials file contains exactly `KRAKEN_API_KEY=...` and `KRAKEN_API_SECRET=...`.
Never print it. Enabling `--order-submission enabled` and any real order require the
user's explicit authorization for that specific test.
