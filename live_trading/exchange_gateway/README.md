# Exchange gateway and real venues

The gateway receives `SubmitOrder`, `CancelOrder` and exchange-snapshot requests from
the execution services and returns order updates, fills and snapshots. It has two
modes (the earlier `hyperliquid-dry-run` plan-inspection mode was removed on 2026-10-15):

| Mode | Behind the gateway |
| --- | --- |
| `backend` | simulated exchange over a private NATS stream (PAPER, replay) |
| `venue` | a real exchange through `venue/LiveVenueAdapter` (currently Kraken Futures) |

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

### Resting long-position stops

An `ExecutionOrder` with `position_stop_price > 0` is a separate reduce-only SELL
stop-market, triggered by the venue mark price. Execution supplies its source-unit
price (70% of the latest daily close) and quantity; the gateway does not calculate or
move that daily level. It converts the price by the symbol's unit scale and rounds
down to `instruments[].tickSize`. Missing/invalid ticks, unusable prices and a rounded
stop at or above the current mark are rejected locally. The existing planned/venue
USD-value check still applies using `reference_price`, not the stop price.

The journal stores `order_type`, the original `position_stop_price` and rounded
`venue_stop_price`. Opening an old journal adds these columns transactionally, with
existing rows defaulting to `MARKET` and zero stop prices. Accepted stops stay `OPEN`
while listed, including across daily cycles and restarts, and are never resent.
The stop's `active_from` is its command's `created_at`.

The migration preserves existing market rows; older gateways do not understand new
stop rows. The release owner must prevent a downgrade to a gateway without stop
support while stops remain pending: an older reader could treat a prepared stop as a
market order. Release/rollback configuration is outside this gateway task.

Execution replaces a stop by submitting its successor first, then cancelling the old
one after acceptance. Multiple open stops on the same symbol are therefore normal;
tracking and cancellation use each order's own client id, never just the symbol.
Fills and snapshot open orders resolve by `cliOrdId` first; when it is absent they
use the journaled venue `order_id`. An explicit foreign/unknown client id does not
fall back to another order's venue id. Fill timestamps still guard against reused ids,
and fill quantities/prices are converted back with the journaled order's unit scale.

Kraken REST references checked on 2026-10-17:

- [Send order](https://docs.kraken.com/api/docs/futures-api/trading/send-order):
  `orderType=stp`, `symbol`, `side=sell`, `size`, `stopPrice`, `reduceOnly=true`,
  `triggerSignal=mark`, `cliOrdId`. No `limitPrice` means stop-market. Acceptance uses
  `sendStatus.status` and `sendStatus.order_id`. Market order bodies are unchanged.
- [Open orders](https://docs.kraken.com/api/docs/futures-api/trading/get-open-orders):
  `openOrders[]` with `order_id`, `symbol`, `side`, `filledSize`, `unfilledSize`.
- [Fills](https://docs.kraken.com/api/docs/futures-api/trading/get-fills): `fills[]`
  with `fill_id`, `order_id`, `symbol`, `side`, `size`, `price`, `fillTime`, `fillType`.
- [Instruments](https://docs.kraken.com/api/docs/futures-api/trading/get-instruments):
  `instruments[].tickSize` supplies the price increment.

The open-order/fill REST examples omit `cliOrdId`; the parser accepts it when present
and does not require it. Actual triggered-stop id continuity, concurrent reduce-only
stops, venue acceptance and live trigger behavior remain to be verified with a real
account and an individually authorized small order. Offline tests use fake transport only.

Fees and funding (the venue reports no fee on fills): the adapter reads the account
history every 5 minutes and 30 s after a new fill. Each fee or funding entry is
journaled once (`cash_entries`, keyed by the venue log id) and published as a
`VenueCostEvent` (`execution.event.venue_cost.v1`): the fee with its local fill id, the
funding with its source coin, the amount as the change of USD cash. A fee listed before
its fill has been polled waits for it (at most 10 minutes, then it is booked on the
account). The first read of a new journal only marks the existing history as seen.
Execution-state applies each cost once (`trading_venue_costs`): cash moves, fees join
their fill's commission and funding its position in the position ledger.

The same path carries the other USD changes of the Multi-M wallet (user decision
2026-10-14): venue interest (`INTEREST`, a cost) and deposits, withdrawals and
conversions (`CAPITAL_FLOW`: capital, not profit or loss). Both stay account-level and
are totalled per day in `trading_equity_daily` (`interest`, `capital_flows`), so the cash
keeps matching Kraken and the dashboard can tell deposits from performance. BTC movements
and other wallets do not change the USD cash; they are logged once as collateral-only. A
USD change of an unknown kind is logged as not booked; reconciliation then reports the
cash difference and pauses trading.

Cash in the snapshot is reported spot-style (USD balance minus the entry value of open
positions), because execution-state's cash falls by the value of a buy. Reconciliation in
the chain's LIVE mode accepts 0.50 USD of difference (`--cash-tolerance-usd`, venue
rounding); PAPER stays exact.

BTC collateral: the snapshot also carries `collateral_usd`, Kraken's own post-haircut USD
value of the non-USD wallet currencies (BTC). Reconciliation does not compare it.
Execution-state fixes the value per day (`trading_daily_collateral`, first use wins, so a
redelivered daily snapshot is identical) and puts it in the daily `AccountSnapshot`;
portfolio risk sizes with cash + positions + collateral (user decision 2026-10-14: "USD +
BTC with haircut"). Orders never spend it. PAPER has none and its messages are unchanged.

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
| Submit | `POST /derivatives/api/v3/sendorder` | `mkt` (IOC with 1% price protection), or `stp` position stop with `stopPrice`, `reduceOnly=true`, `triggerSignal=mark`; common `symbol`, `side`, `size`, `cliOrdId` |
| Cancel | `POST /derivatives/api/v3/cancelorder` | by `cliOrdId`; `cancelled` / `filled` / `notFound` |
| Open orders | `GET /derivatives/api/v3/openorders` | resting-order and recovery evidence; `order_id`, optional `cliOrdId` |
| Fills | `GET /derivatives/api/v3/fills` | last 100, newest first; `order_id`, optional `cliOrdId`; **no fee field** |
| Fees, funding, interest, capital flows | `GET /api/history/v2/account-log?count=500` | newest 500 entries; `fee` (paid, positive) with `execution` = fill id; `realized_funding`; other USD changes of the `flex` wallet = `new_balance - old_balance`: `info` with "interest" → INTEREST, with "transfer"/"conversion" → CAPITAL_FLOW; read every 5 min and 30 s after a fill |
| Snapshot | `GET accounts`, `openpositions`, `openorders` | cash = USD quantity of the Multi-M (`flex`) wallet minus Σ position size x `price` (average entry); collateral = Σ `collateral` (USD after haircut) of the non-USD currencies |
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
Confirmed on 2026-10-15 with the read-only key: the account monitor read the account
history (38 rows, including the user's own small PF_XBTUSD/PF_HYPEUSD trades of
2026-10-07) and one account snapshot. The parser test "real account history shape" uses
that row shape with synthetic ids.
- Account history path and signing (query `count=500` as `postData`, no nonce); body `logs`.
  Every row has all fields, `null` when unused.
- `info` words seen: "futures trade", "conversion", "cross-exchange transfer" (BTC moved
  in/out of the spot wallet; contains "transfer", so a USD one becomes CAPITAL_FLOW).
- `old_balance`/`new_balance` are the balance of the row's `asset` before/after: "usd",
  "xbt", or the contract (e.g. "pf_xbtusd"), whose balance is the signed position size.
- A trade has a USD row and a contract row. On the USD row `fee` is positive and paid in USD,
  `execution` is set, and new = old + `realized_pnl` - `fee` + `realized_funding`: realized
  PnL settles into the USD balance at once, and `realized_funding` is the change of the USD
  balance (positive = received; seen +0.0106 USD on a reduced short). Opening a position
  moves the USD balance only by its fee.
- With no USD, Kraken converts a little BTC itself to pay a fee: a "conversion" pair (USD +x,
  XBT -y) at the same time as the fee. Booked as CAPITAL_FLOW +x and Fee -x (net zero); the
  BTC side changes only the collateral.
- Collateral: `accounts.flex.currencies.XBT.collateral` = `value` x (1 - 1% haircut); margin
  equity = BTC collateral + USD.

Still open:
- Interest and plain USD deposit/withdrawal words (none in the history yet); that `id` is an
  integer (the monitor's `account-log-summary.json` now records field types).
- That `execution` equals the `fill_id` of `/fills`, and that `openPositions[].price` is the
  average entry price (no open position yet).
- Periodic funding rows ("funding rate change") while a position stays open: the history
  shows funding realized only when the position was reduced.

## Run and test

```bash
# Offline: recorded responses only.
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

In the chain this is the `gateway-kraken` service, started by `MODE=live` in chain.env
(deploy/chain/README.md, "Switching to live"); `LIVE_ORDER_SUBMISSION` maps to
`--order-submission`. Execution-state sets `reference_price` on every order (the daily
plan's close(T), or the current Binance price for a Manual Control order), including
orders recovered after a restart, so the USD value check always has its planning price.
