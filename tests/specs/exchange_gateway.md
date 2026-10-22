# Exchange gateway: real-venue adapter, Kraken Futures protocol, symbol map, venue journal

Scope: `live_trading/exchange_gateway/src/venue` linked as `exchange_gateway_venue_dep`:

| Part | File | What the trading system needs from it |
| --- | --- | --- |
| Symbol map | `symbol_map.*` | the only path from a strategy symbol (Binance) to a venue contract and its units |
| Kraken Futures protocol | `kraken_futures_protocol.*` | documented request signing, request bodies, response parsing and status mapping |
| Live venue adapter | `live_venue_adapter.*` | submission gate, journal-before-send, no blind resend, fill de-duplication, unit conversion, events |
| Venue journal | `venue_journal.*` | durable orders, fills and cash entries (SQLite) that make restarts safe |

Sources: `live_trading/exchange_gateway/README.md` (Safety rules, Kraken Futures mapping,
Symbol map, Resting long-position stops), the headers' comments, `docs/codex/STOP_LOSS_DESIGN.md`,
and the Kraken Futures REST authentication guide (as summarised in the README).

The outside world is faked: a scripted `HttpTransport` returns recorded Kraken-shaped
answers per endpoint and records every request. No request leaves the process; the Kraken
secret is a synthetic Base64 string. Technical time is an explicit fake clock; the business
day is an explicit `YYYYMMDD`.

Units: source = Binance perpetual units (1 `1000PEPEUSDT` = 1,000 PEPE); venue = the Kraken
contract's underlying units. `venue quantity = source quantity x scale`,
`venue price = source price / scale`; the USD value is the same on both sides.

---

## Part S. Symbol map

### S1 Explicit units

Entry `1000PEPEUSDT -> PF_PEPEUSD, scale 1000`: `bySource("1000PEPEUSDT")` gives that entry,
`byVenueSymbol("PF_PEPEUSD")` gives it back. 5 source units = 5,000 PEPE; a source price of
0.01 USD = 0.00001 USD per PEPE.

### S2 Unmapped symbols are not tradeable

`bySource("PEPEUSDT")` (not in the table) and `byVenueSymbol("PF_UNKNOWNUSD")` give nothing;
names are never guessed (no stripping of `1000`, no `USDT -> USD`).

### S3 The table is checked when built

Empty venue, an empty field, a scale that is zero, negative or not finite, the same source
twice, or two sources mapped to one venue contract -> `std::invalid_argument`.

### S4 Loading the file

`schema_version` must be 1, `venue` must equal the expected venue, `symbols` must be a list
and every entry must have its four typed fields; otherwise `std::runtime_error` (also for a
missing file or invalid JSON). The committed `config/venues/kraken_futures/symbol_map.json`
loads for `kraken-futures`: `BTCUSDT -> PF_XBTUSD` x1 and `1000PEPEUSDT -> PF_PEPEUSD` x1000.

---

## Part K. Kraken Futures protocol

### K1 Signing (Kraken's documented algorithm)

`Authent = Base64(HMAC-SHA512(key = Base64Decode(secret), message = SHA256(postData + nonce +
path)))`, where `path` is the endpoint path **without** the `/derivatives` prefix
(`/api/v3/sendorder`). The test computes the expected value independently with OpenSSL.
A secret that is not valid Base64 is refused.

### K2 Private POST (sendorder, cancelorder)

URL `https://futures.kraken.com/derivatives/api/v3/<endpoint>`, form body, headers `APIKey`,
`Nonce` (milliseconds, strictly increasing between requests) and `Authent` =
K1(secret, body, nonce, `/api/v3/<endpoint>`).

### K3 Private GET (openorders, fills, accounts, openpositions)

No `Nonce` header; `Authent` = K1(secret, "", "", `/api/v3/<endpoint>`). GET is the only
method of every read request.

### K4 Account history GET

`https://futures.kraken.com/api/history/v2/account-log?count=500`, signed with the query
`count=500` as postData, no nonce, path `/api/history/v2/account-log`.

### K5 Market order body

`orderType=mkt&symbol=PF_XBTUSD&side=buy&size=0.0123&cliOrdId=algo-t1-7` (size printed exactly,
no trailing zeros). An empty or non-positive size or a client id over 100 characters is refused.

### K6 Position stop body (stop-market, reduce-only, mark-triggered)

`orderType=stp`, `side=sell`, `size`, `cliOrdId`, `stopPrice=<price>`, `reduceOnly=true`,
`triggerSignal=mark`, and **no** `limitPrice`. A BUY stop or a non-positive stop price is
refused before any request is built.

### K7 Submit answers

| Answer | Outcome |
| --- | --- |
| `sendStatus.status` `placed`, `partiallyFilled`, `filled` | Accepted, with `sendStatus.order_id` |
| HTTP 429, `apiLimitExceeded`, `nonceBelowThreshold`, `nonceDuplicate` | NotProcessed (safe to resend) |
| no HTTP answer, HTTP 5xx, `Unavailable`, `Server Error`, `unknownError`, an error without code, `clientOrderIdAlreadyExist` | Unknown (never resent blindly) |
| other HTTP 4xx, other error codes, other statuses (e.g. `insufficientAvailableFunds`) | Rejected |

### K8 Cancel answers

`cancelled` -> Cancelled, `filled` -> AlreadyFilled, `notFound` -> NotFound, anything else or
no answer -> Unknown.

### K9 Markets

Only `PF_` flexible futures quoted in USD and not `tradfi`. `XBT` is reported as `BTC`.
Quantity step = 10^-`contractValueTradePrecision` (4 -> 0.0001, 0 -> 1). Not tradeable,
expired, `contractSize` != 1 or no usable `tickSize` -> listed but not trading, with a reason.
No PF market at all -> `ProtocolError`.

### K10 Mark prices

`markPrice` per `PF_` symbol; suspended contracts and non-PF symbols are left out.

### K11 Fills and open orders

Fill: `fill_id`, `order_id`, optional `cliOrdId`, `symbol`, `side`, `size`, `price`,
`fillTime` -> Unix ms and UTC day (`2026-10-12T00:00:07.250Z` -> day 20261012, ms
1,791,763,207,250). Non-positive size/price or a non-UTC time -> `ProtocolError`. Open order:
`order_id`, optional `cliOrdId`, `symbol`, `side`, `filledSize`, `unfilledSize`.
A `"result": "error"` envelope -> `ProtocolError`.

### K12 Account

USD balance = `accounts.flex.currencies.USD.quantity`; collateral = sum of `collateral` of the
other currencies; positions signed (`short` negative) with their entry `price`. A wallet that is
not `multiCollateralMarginAccount` -> `ProtocolError`.

### K13 Account history -> cash entries

USD rows only: `fee` 0.75 with `execution` E -> Fee -0.75 for fill E (id `<id>:fee`);
`realized_funding` +0.0106 -> Funding +0.0106 (`<id>:funding`); "interest" with balance
1000 -> 999.5 -> Interest -0.5; "cross-exchange transfer" 0 -> 500 -> CapitalFlow +500;
"futures trade" without fee/funding -> nothing; a non-USD change -> collateral only
(`not_cash`); an unknown USD change -> `unaccounted`, never booked. The funding contract name
is upper-cased (`pf_strkusd` -> `PF_STRKUSD`).

### K14 Permissions and client ids

`general = FULL_ACCESS` -> read and trade; `READ_ONLY` -> read only; any other permission that
is not `NO_ACCESS` (transfer, withdrawal) -> withdraw flagged. Client id
`algo-<namespace>-<order id>`; the namespace is 1-24 lowercase letters/digits; ids of another
namespace are not ours. The host must be HTTPS; the key and secret formats are checked.

---

## Part A. Live venue adapter

Fake venue: markets PF_XBTUSD (step 0.0001, tick 1, mark 62,000), PF_ETHUSD (step 0.001,
tick 0.1, mark 2,500), PF_PEPEUSD (step 1, tick 1e-10, mark 0.00001), PF_OLDUSD (expired);
symbol map BTCUSDT x1, ETHUSDT x1, 1000PEPEUSDT x1000, OLDUSDT x1. Business day 20261012.

### A1 Submission is disabled by default: GET only

With default options every order is rejected locally ("order submission disabled"), a
Rejected update is published, and no POST is ever sent. Through `GetOnlyTransport` a POST
cannot leave the process (it throws), while GETs pass.

### A2 Journal before send

After `submitOrder` the order is in the journal as PREPARED and nothing was sent. When the
`sendorder` request leaves, the journal already says SENDING.

### A3 Accepted orders are reported once

`placed` -> journal OPEN with the venue order id; one Accepted `OrderUpdateEvent` whose
message id is deterministic (`kraken-futures-order:<id>:OPEN`).

### A4 Held until its business day

`active_from = 20261013` is not sent on 20261012; it is sent on the first poll of 20261013.

### A5 Market checks before journaling

Unmapped source -> `MARKET_UNAVAILABLE`; mapped but expired -> `MARKET_NOT_TRADING`;
0.00004 BTC (below step 0.0001) -> `BELOW_MINIMUM_QUANTITY`. Each is a Rejected update and
nothing is sent.

### A6 USD value check (1.5x)

Planned USD = source quantity x `reference_price`; venue USD = venue quantity x mark.
0.1 BTC planned at 62,000 (6,200 vs 6,200) passes; planned at 45,000 (ratio 1.378) passes;
planned at 40,000 (ratio 1.55 > 1.5) -> `NOTIONAL_MISMATCH`; planned at 95,000 (ratio
0.653 < 1/1.5) -> `NOTIONAL_MISMATCH`. A 1000PEPEUSDT order with the scale wrongly set to 1
(5 PEPE worth 0.00005 USD vs 0.05 planned) -> `NOTIONAL_MISMATCH`. No reference price ->
`NO_REFERENCE_PRICE`; no mark for the contract -> `NO_VENUE_PRICE`.

### A7 Quantities rounded to the nearest venue step

0.12349 BTC -> `size=0.1235`, 0.12344 -> `size=0.1234` (user decision 2026-10-22: rounding
down kept small accounts below their targets). Less than half a step is not sent
(`BELOW_MINIMUM_QUANTITY`). A position stop is still rounded down: it never covers more than
the position. Execution-state accepts the resulting overfill (up to 2x the order) only with
`--venue-lot-overfill yes` (LIVE).

### A8 Units converted both ways

5 `1000PEPEUSDT` (reference 0.01) -> `size=5000` on PF_PEPEUSD. A venue fill of 5,000 PEPE at
0.00001 is published as 5 `1000PEPEUSDT` at 0.01 (USD 0.05 unchanged), commission 0 (the fee
follows as a venue cost), with the strategy id and order id of the command.

### A9 Duplicate commands

The same order id with the same terms -> ignored (one `sendorder`). The same id with different
terms -> `std::invalid_argument`.

### A10 Ambiguous outcomes are never resent

No HTTP answer or HTTP 5xx -> UNKNOWN; later polls send nothing for it (even after the
2-minute alert). It becomes OPEN when the venue lists it in open orders, or FILLED from its
fills.

### A11 "Not processed" is resent safely

HTTP 429 -> back to PREPARED; the next poll sends it again with the **same** client order id;
then `placed` -> OPEN.

### A12 Restart from the journal

A journal order in SENDING at start (crash during send) becomes UNKNOWN and is not resent.
A PREPARED order is sent once by the new adapter. An OPEN order is not resent, and its fill
seen after the restart is published once.

### A13 Fills de-duplicated by venue fill id

The same venue fill listed by two polls -> one `FillEvent`. Local fill ids are 1, 2, ...
in arrival order. Fill events come before the order's Filled update.

### A14 Partial fills

0.1 BTC: fill 0.04 -> journal OPEN (partial), FillEvent 0.04; fill 0.06 -> FILLED,
FillEvent 0.06, then the Filled update; sum of published quantities = 0.1.

### A15 Fills that are not ours are not attributed

A fill with another namespace's client id, or without client id and with an unknown venue
order id, produces no `FillEvent`.

A fill **without** client id whose venue order id is journaled for one of our orders (same
symbol and side) is ours (the Kraken REST examples omit `cliOrdId`). A fill carrying our client
id but dated more than a minute before the order was sent (a reused client id) is not
attributed.

### A16 Cancels

A PREPARED order is cancelled locally (no request) and reported Canceled. An OPEN order gets
a `cancelorder` POST by client id; `cancelled` -> Canceled update.

### A17 Position stops

SELL 0.1 BTC, `position_stop_price` 43,400.7, reference 62,000 -> `orderType=stp`,
`stopPrice=43400` (rounded **down** to tick 1), `reduceOnly=true`. A stop at or above the
mark (63,000 with mark 62,000) -> `STOP_NOT_BELOW_MARK`, nothing sent. A BUY stop ->
`INVALID_POSITION_STOP`.

### A18 Key permissions

A key with withdrawal access is refused; with submission enabled a key that cannot trade is
refused.

### A19 Snapshot in source units, spot-style cash

USD balance 10,000, long 0.1 PF_XBTUSD entered at 60,000, short 2,000 PF_PEPEUSD entered at
0.00001: positions BTCUSDT +0.1 and 1000PEPEUSDT -2; cash = 10,000 - (0.1 x 60,000 +
(-2,000) x 0.00001) = 4,000.02; collateral passed through.

### A20 Fees and funding from the account history, each published once

The first read of a new journal is a baseline: existing entries are marked as seen and not
published. Afterwards a fee of 0.75 USD on our fill F1 is published once as a
`VenueCostEvent` Fee of -0.75 with the local fill id of F1 and the order's source coin; a
funding entry on `pf_xbtusd` of -0.20 is published as Funding -0.20 on `BTCUSDT`. Reading the
same history again publishes nothing new.

### A21 An open order that disappears ends

An OPEN order that the venue no longer lists, with no fills, for 15 s (`settle_after_ms`)
is reported Canceled; with a partial fill it is reported Canceled after its fills (the
remainder ended).

---

## Part J. Venue journal

### J1 Bound to one venue and account namespace

Re-opening the file with another namespace (or venue) is refused.

### J2 Durable across reopen

An order and its fills survive closing and reopening the journal; local fill ids continue
(1, 2 -> next 3); `knowsFill` remembers venue fill ids.

### J3 Cash entries recorded once

`recordCashEntry` twice with the same id stores one entry; it is unpublished until marked.

---

## Findings

Run 2026-10-19: 79 tests, 79 pass, none disabled. The code agrees with every rule above;
no finding. (Rules A15 fallback/reused id, A20 and A21 were added after the first run, from the
README's safety rules, and passed on their first run.)
