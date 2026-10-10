# Live Trading

This directory contains the executable services that connect the trading domain library to
market data, durable messaging, persistence, planning, and exchange execution.

Start at each service's `*_main.cpp`: its real startup, recovery, subscriptions and service
loop remain visible there. Follow its domain components into `lib/` for trading formulas
and reusable behavior. Separate service files exist for actual responsibilities, such as
ingestion and HTTP serving. Each service has one `src/meson.build`; the top-level
build enters it directly without an intermediate forwarding build file.

PAPER defaults to quote-notional liquidity: actual Binance daily quote turnover,
SMA 25, top 20 inside the exchange top-50 candidate set. The ingestor stores and
backfills `ohlcv_data.quote_volume` for completed candles; invalid quote data prevents
publication. The strategy refuses missing required quote history. Ordinary LIVE and
the canonical historical profile keep their existing configuration. Existing PAPER
upgrades require a backed-up, checked configuration-identity migration preserving
completed checkpoints; see `deploy/paper_trading/README.md`.

## Start here

Read the services in this order if you are new to the project:

1. `market_data_service` — gets the latest completed daily market data and commits it to SQLite.
2. `strategy_service` — reads committed history and publishes strategy intents.
3. `portfolio_risk_service` — combines strategy intents, account state, sizing, and risk rules.
4. `execution_state_service` — owns durable execution/account state and coordinates each execution cycle.
5. `order_planner_service` — converts a notional planning request into an executable notional plan.
6. `exchange_gateway` — forwards exchange commands and normalizes exchange events.
7. `simulated_exchange_service` — durable exchange backend used when a real exchange is not used.
8. `historical_market_data_service` — historical/replay source that preserves the same anti-lookahead boundary.
9. `binance_simulator` — standalone local HTTP simulator for Binance-style market-data requests.
10. `execution_service` — direct execution-engine service path; inspect deployment/configuration before assuming it replaces `execution_state_service`.

## Service message flow

The full service/component execution chain is shown below. The current default
`deploy/live/` topology stops at `NotionalOrderPlan`; its execution-state
service does not submit private orders. The gateway is an optional transport-only
profile. Treat the downstream chain as a component responsibility map, not proof
that the default LIVE deployment has exchange execution connected.

The separate [paper deployment](../deploy/paper_trading/README.md) connects these
services to the simulated exchange using current public data. Opt-in ingestion
publishes only open(T+1) for execution, outside completed feature bars. It does not
enable private orders in default LIVE.

PortfolioRisk service adds nullable `risk_diagnostics_payload` to its decision
checkpoint and writes it atomically with the existing signals/account/decision
payloads. New cycles record sizing, strategy constraint reductions, volatility
availability and configuration identity. Publish/checkpoint/ACK ordering and control
messages are unchanged. Existing rows remain readable without manufactured reports;
the dashboard reads optional payloads compatibly with older schemas. Runtime retries
reject a changed configuration file rather than mixing identities within a cycle.

The chain is:

```text
Market data commit
    -> MarketDataUpdated
    -> Strategy service
    -> StrategyIntentBatch
    -> Portfolio/risk service  <--- AccountSnapshot
    -> DecisionBatch
    -> Execution-state service
    -> NotionalOrderPlanningRequest
    -> Order-planner service
    -> NotionalOrderPlan
    -> Execution-state service
    -> exchange commands through MessageExchange
    -> Exchange gateway
    -> exchange backend / simulated exchange
    -> OrderUpdate + Fill + ExchangeSnapshot
    -> Execution-state service
    -> updated AccountSnapshot / ExecutionCycleComplete
```

The exact contract types and subjects are defined outside this directory. Do not infer a
contract from a filename: follow the concrete message type and `MessageSubjects` value used
by the service.

## What each service owns

| Service | Main responsibility | Important inputs | Important outputs |
| --- | --- | --- | --- |
| `market_data_service` | Daily live ingestion | Binance REST + config | SQLite commit + `MarketDataUpdated` |
| `historical_market_data_service` | Anti-lookahead replay feed | Historical CSV + business time | SQLite commit, `MarketDataUpdated`, execution-open prices |
| `strategy_service` | Strategy evaluation | `MarketDataUpdated` + canonical history | `StrategyIntentBatch` |
| `portfolio_risk_service` | Portfolio sizing/risk decisions | Strategy intents + account snapshot + history | `DecisionBatch` |
| `order_planner_service` | Order-plan construction | `NotionalOrderPlanningRequest` | `NotionalOrderPlan` |
| `execution_state_service` | Durable execution/account state | decisions, plans, market updates, exchange events | account snapshots, planning requests, execution-cycle completion |
| `exchange_gateway` | Exchange boundary | submit/cancel/snapshot commands | normalized order/fill/snapshot events |
| `simulated_exchange_service` | Durable simulated backend | execution prices + backend commands | backend order/fill/snapshot events |
| `execution_service` | Direct execution-engine path | decisions, prices, order updates, fills | persisted trading state / exchange actions |
| `binance_simulator` | Local market-data HTTP simulation | historical CSV | Binance-compatible HTTP responses |

## Local shadow observer

`shadow_observer.py` consumes exported CURRENT `NotionalOrderPlanBatch` payloads;
it does not recompute strategy/risk/planning or update trading accounts. A separate
SQLite journal binds the source risk configuration and immutable producer message/payload
identities, retains retryable unavailable reads and resumes without new observations
for already completed plans. Only new observations require a fresh fixture snapshot.
The risk fingerprint does not cover a full strategy/market/venue manifest.

The preparation reader accepts one numeric loopback HTTP endpoint, uses only
`GET /snapshot`, ignores proxy settings and rejects redirects. It has no credentials,
private submit/cancel methods or event-publishing path. Fixture quantity rounding and
marks are diagnostics; they do not modify producer quantities or confirm fills.
Fees, funding, collateral, FX and PnL remain unavailable. This is local preparation,
not Kraken/Hyperliquid integration or live order readiness.

Run the integrated rehearsal under WSL with local Docker and current packaged images:

```bash
python3 validation/shadow_trading_test.py
```

It reuses the bounded PAPER fixture and exports actual producer input, observations,
HTTP calls and offline `report.html`/JSON under ignored
`storage/paper_validation/<random-project>/shadow/`. Only its own containers/networks
are removed afterward; evidence and volumes remain. No normal PAPER or VPS changes.
See [validation](../validation/README.md#local-shadow-rehearsal) for prerequisites/limits.

## Kraken Multi-M preparation

The user selected Kraken as the initial integration target and wants mixed BTC/USD
collateral: hold BTC as collateral and keep USD to cover charges/losses. The user
chose a USD 50 net account floor and at least 10% available USD, as defined below.
BTC remains collateral, not
spendable USD or an automatically sold strategy position. The current core `Account`
still owns USD cash and filled positions; it is not a Multi-M derivatives wallet.
Private balances, margin, funding and settlement need integration before real trading.

`kraken_shadow.py` reads only Kraken HTTPS public `GET instruments` and `GET tickers`,
with fixed live/legacy-demo hosts, bounded responses and no redirects/proxies. It
normalizes PF_ linear USD perpetuals, keeps inactive contracts visible, rejects
incomplete/stale metadata and distinguishes missing books from unsupported markets.
Quantity precision includes negative values such as PEPE's 1000-unit step. CURRENT
USD notional intent is observed against the current Kraken mark; the Binance
reference close and immutable producer plan are retained. No historical close/USDT
value is silently converted to USD, and scaled source-contract quantities are not
reused. These quantities are public diagnostics, not executable orders or fills.

Run under WSL for a public snapshot and ignored offline HTML/JSON evidence:

```bash
python3 live_trading/kraken_shadow.py
```

Optional `--plans <producer-input.json> --configuration-fingerprint <persisted-risk-sha256>`
observes exported CURRENT plans using the existing durable observation journal.
`--output storage/kraken_shadow/<study>` reuses separate state on restart. The journal
binds venue/environment/schema and the source risk fingerprint, rejects source/payload
conflicts, retains completed observations and retries unavailable reads. Every capture
retains raw bytes, GET receipts/hashes, normalized snapshot and source plans. This is
not a strategy-wide manifest or forward/backtest acceptance. Historical fixture plans
retain their dates and are not current strategy signals.

For the explicitly hypothetical cross-margin BTC/USD diagnostic, create ignored
`storage/kraken_shadow/scenario.json`. The values below are an example only, not the
user's balances or a recommended allocation. Cash debits represent separate assumed
settled charges; do not also include them in unrealized PnL/funding.

```json
{
  "scope": "HYPOTHETICAL_CROSS_MARGIN_SCENARIO",
  "profit_currency": "USD",
  "btc_balance": "0.1",
  "usd_balance": "2000",
  "unrealized_pnl_usd": "-100",
  "unrealized_funding_usd": "-1",
  "initial_margin_usd": "500",
  "maintenance_margin_usd": "250",
  "cash_debits_usd": "50",
  "minimum_usd_reserve": "500"
}
```

```bash
python3 live_trading/kraken_shadow.py --collateral-scenario storage/kraken_shadow/scenario.json
```

The diagnostic values BTC at the current index, applies the reviewed 1% haircut,
and separates estimated margin equity from USD remaining after assumed charges and
unrealized losses. It flags low USD reserves, uncovered losses and margin shortfalls;
it never sells BTC, approves trading or calculates actual venue liquidation levels.
Margin totals are required hypothetical inputs, not inferred from generic leverage.
Isolated/unified wallets, other assets, orders and real margin schedules are outside
this diagnostic. Private venue margin/balances remain authoritative when integrated.

Official [EEA collateral rules](https://support.kraken.com/articles/collateral-currencies-eea)
list BTC at a 1% haircut and 0.2% conversion fee. A haircut reduces usable collateral;
it is not a fee deducted from the BTC balance. Keep USD settlement and enough USD
for losses/trading fees/funding to reduce conversions; USD does not eliminate trading
fees or funding and can be depleted. Uncovered unrealized losses can incur interest.
Refresh [charges](https://support.kraken.com/articles/4844392809620-fees-charges-for-multi-collateral-derivatives)
and [account eligibility](https://support.kraken.com/articles/derivatives-eligibility-requirements-eea)
before private integration; no keys are consumed by this public reader.

For the next authenticated account inspection, create a **Derivatives** API key
with **General API: Read Only** and **Withdrawal API: No Access**, following the
[official key guide](https://support.kraken.com/articles/360022839451-how-to-create-an-api-key-for-kraken-derivatives).
Spot API keys are separate. Credentials must stay in ignored local storage and never
in chat, tracked configuration or public reports. The current public reader does not
consume keys. The separate account reader below loads them internally only.
User-confirmed Spain/Futures activation does not establish the actual wallet type,
BTC collateral or private API access. No funding transfer is needed for key creation.

`kraken_account.py` prepares separate authenticated read-only account inspection.
Five fixed GET routes check read-only/no-transfer permissions before account reads
and reject redirects, stale/non-UTC clocks and incomplete responses. The independent
SQLite journal binds account/environment/policy and handles duplicate delivery and
restart without creating fills or updating the core account. Venue flex-wallet
collateral, margin equity and margin including open orders are retained directly.
USD reserve checks are conservative stress diagnostics, not conversion forecasts
or trading approval. Missing balances/policy and non-USD settlement require review.
Sequential reads are not atomic. Real read-only account inspection passed; actual
remaining venue/accounting/recovery acceptance, forward venue shadow and private order acceptance remain pending.

For real read-only inspection, put only these two variables in the dedicated
ignored UTF-8 file storage/kraken_shadow/.env: KRAKEN_API_KEY and KRAKEN_API_SECRET.
Use one name=value assignment each; optional matched quotes and comments are allowed.
The program reads the file internally without exposing its contents. Never inspect,
print or source credential files through diagnostic shell commands. Run:

```bash
python3 live_trading/kraken_account.py --account
```

The default output is storage/kraken_shadow/account-live; fixture output/state remains
separate. Policy is explicit; omitting --policy still requires review. The user's
policy is retained in ignored storage/kraken_shadow/account-policy.json:

```json
{
  "minimum_account_equity_usd": "50",
  "minimum_usd_reserve_fraction": "0.10",
  "assumed_future_cash_debits_usd": "0"
}
```

The basis is reported marginEquity in USD, including venue collateral discounts
and position effects, never leveraged notional. USD 50 net equity requires USD 5
available cash; USD 1000 requires USD 100. Available USD is the lesser of quantity
and availability. Low equity and low current USD are independent diagnostics;
projected loss/charge stress is labelled separately, without double-counting PnL
in equity. Zero additional assumed debit does not complete future cost forecasting
or eliminate real fees/funding. No automatic rebalance or exchange stop is performed.

A changed policy requires a separate journal to preserve earlier observations:

```bash
python3 live_trading/kraken_account.py --account --policy storage/kraken_shadow/account-policy.json --output storage/kraken_shadow/account-live-policy-50-10
```

Old illustrative fixed thresholds use minimum_usd_reserve and
assumed_future_cash_debits_usd; never substitute them for the user's policy.
`kraken_account_monitor.py` polls the same read-only account checks every 30 seconds
and exports durable warning/recovery events for the existing Go Telegram notifier.
Run locally under WSL, with the already-built algotrading-telegram-api image:

```bash
python3 live_trading/kraken_account_monitor.py --telegram
```

The account loader consumes storage/kraken_shadow/.env internally. Docker consumes
storage/paper_trading/.env for Telegram; never display either file or dump container
environment variables. Default policy is storage/kraken_shadow/account-policy.json.
Default state is storage/kraken_shadow/account-alerts/: account-observations.sqlite,
events.jsonl, status.json, monitor.pid and notifier.json (the local container name).
A nonblocking process lock prevents two producers using that directory.
Warnings cover the account floor, USD availability and initial/maintenance margin.
Current and stressed USD shortages share one warning. Repeated snapshots and
restarts do not repeat unchanged warnings; a fresh valid recovery sends a resolution,
and recurrence opens a new event. Invalid/stale/failed reads never resolve alerts.
Heartbeat freshness gates the notifier. Existing durable notifier decisions/receipts
handle restarts; ambiguous network delivery is not an exactly-once guarantee.
Ctrl+C stops the foreground observer; stop its named Docker notifier separately
if desired. The notifier cannot deliver from an expired heartbeat. This is a local
monitor: the PC/WSL/Docker and network must remain running. It is not installed on
the VPS and does not start the observer automatically after a PC reboot. Observation
history grows with polling; plan retention before installing a permanent service.
These alerts do not stop entries, close positions or sell BTC.

The user-selected account policy is warning-only: net equity below USD 50, available
USD below 10% of net equity, a decline of at least 10% versus the preceding UTC day's
last observed equity, and gross open-position exposure above 5x current net equity.
There is no independent daily loss stop or peak drawdown stop. Raw equity comparisons
include BTC price changes and transfers; they are not trading-only loss/PnL. Missing
previous-day/nonpositive baseline equity is unavailable. Gross exposure sums absolute
LONG and SHORT USD marked notionals, never netting opposite sides or substituting
entry prices; pending orders are not confirmed exposure. Missing pricing/nonpositive
equity is unavailable rather than a fabricated ratio/recovery. Those checks retain
existing active alerts when unavailable; status.json lists unavailableChecks.
Defaults --daily-equity-drop-fraction=0.10 and --gross-exposure-warning=5 are
configurable. Observer/read failures and margin warnings do not issue any stop or
liquidation/closing command. Existing strategy constraints remain independent:
pure_rsi_equal_weight.json targets 10% per full signal and scales gross targets to
1.5x. The new 5x venue-account warning does not change historical sizing/economics.
The user cancelled additional opening-order admission/limit work. Preserve existing
order planning, sizing and constraints; do not add a separate 10% guard or seek
further clarification about it. Actual forward shadow is the next task.

Telegram uses compact importance/account/reason messages. Optional --account-name
changes the display label (default Kraken); identifiers remain in local evidence.
The monitor also sends one INFO daily portfolio on the first valid observation at
or after --summary-hour-utc (default 9, UTC). Repeated polls/restarts do not resend
that day's summary and offline days are not backfilled. Balance is the sum of venue
currency value fields in USD before collateral discounts; equity is authoritative
venue marginEquity. They are not interchangeable. Active linear Multi-M positions
show absolute USD exposure at fresh verified public mark prices, with LONG/SHORT
direction, plus exposure as a percentage of that snapshot's positive net equity,
rather than token units or average entry prices. Percentages can exceed 100% and
do not describe funded margin. Nonpositive equity leaves the percentage unavailable.
Yesterday's percentages retain yesterday's snapshot equity, not today's denominator.
Existing historical formatted snapshots are preserved without invented percentages.
Missing/unsupported/stale
pricing is explicitly unavailable. When positions exist, public instruments/tickers
are read alongside each account observation to preserve the last day's valuation.
Yesterday's list is the last observed snapshot from the preceding UTC calendar day,
with its timestamp; no prior-day observation is reported as unavailable, never zero.
It is a holdings snapshot, not a history of every position opened/closed during a day.
Summary snapshots and the once-per-day ledger share the durable account event
transaction. The isolated account notifier accepts INFO; operational sources retain
their configured severity thresholds. Its local container is recreated on image
change while keeping the named notification/receipt volume.
`kraken_portfolio_summary.py` owns daily USD valuation and calendar persistence.
Reference: [open positions](https://docs.kraken.com/api-reference/account-information/get-open-positions)
return size and average entry price; current marks come from the separate public reader.

Normalized reports contain account data and remain ignored. last_attempt.json records
our safe status wording only. HTTP 401/403 are distinguished without logging response
bodies/headers. An explicit application User-Agent resolved the initial observed 403;
403 alone does not prove that the keys are invalid. Real orders are never submitted.

Generate a separate bounded offline example and inspect report.html:

```bash
python3 validation/kraken_account_test.py --study storage/kraken_shadow/account-local
python3 live_trading/kraken_account.py --fixture storage/kraken_shadow/account-local/fixture.json --policy storage/kraken_shadow/account-local/policy.json --output storage/kraken_shadow/account-local
```

Example balances and reserve thresholds are illustrative. Fixtures become stale
after 30 seconds; regenerate the study to obtain fresh clocks. Production freshness
checks remain enabled. Reference: Kraken's [authentication specification](https://support.kraken.com/articles/360022635592-generate-authentication-strings-rest-api-derivatives),
[permissions](https://docs.kraken.com/api-reference/api-keys/check-v3-api-key),
[wallets](https://docs.kraken.com/api-reference/account-information/get-wallets),
[positions](https://docs.kraken.com/api-reference/account-information/get-open-positions),
[orders](https://docs.kraken.com/api-reference/order-management/get-open-orders) and
[PnL preferences](https://docs.kraken.com/api-reference/multi-collateral/get-pnl-currency-preferences).

The legacy demo endpoint redirected during the current probe. Its
[official testing guide](https://support.kraken.com/articles/360024809011-api-testing-environment-derivatives)
also announces decommissioning. `--environment demo` remains an explicit public probe
that fails without following redirects or falling back to live. Establish an available
supported test environment before private lifecycle tests. VPS rollout and real orders
remain separate, unaccepted work. Validation/evidence are in CURRENT_STATE.md.

## Market-data files

Within `market_data_service/src/`, filenames describe their local responsibility:

| Files | Responsibility |
| --- | --- |
| `market_data_service_main.cpp` | Startup, daily schedule, retries and shutdown |
| `config.h/.cpp` | Configuration values and JSON validation |
| `binance_client.h/.cpp` | Binance REST calls and bounded parallel downloads |
| `ingestion.h/.cpp` | One daily refresh, validation and commit coordination |
| `market_store.h/.cpp` | Canonical SQLite transactions and tracked-symbol history |
| `update_publisher.h/.cpp` | Durable notification after a successful commit |

The historical feed shares `market_store` so both writers use the same schema and commit
rules. The HTTP simulator keeps its own candle store and HTTP server; it serves requests
rather than writing the canonical database. These are useful responsibility boundaries,
so they remain separate files. Service entrypoints retain their descriptive `*_main.cpp`
names for navigation and operational tooling.

## Time has two meanings

This code deliberately distinguishes two time domains:

- **Business time** decides which market day is economically visible. `TimeHandler` controls it.
- **Technical time** controls polling, retry backoff, and process responsiveness. It uses real wall time.

Never use technical sleeps to decide whether a candle, signal, order, or fill is economically
allowed. Never accelerate retry/backoff just because replay business time is accelerated.

## Durability and message handling

When reading a message handler, look for the return value:

- `Ack` means the message was handled durably and does not need redelivery.
- `Retry` means the service is not ready or hit a recoverable failure; redelivery is required.
- `Terminate` means the message is invalid for this consumer and retrying it would not help.

For durable routes, the ordering depends on which state the service owns:

- Market-data ingestion commits SQLite before publishing its update notification.
- Strategy, portfolio/risk, and order-planner services publish deterministic output, then
  checkpoint, then acknowledge their input. A crash between publish and checkpoint causes
  the same logical message to be published again.
- Execution persists local order/cancel intent before dispatching exchange commands.
- With PostgreSQL persistence, the simulated backend commits account/order state and its
  outbox together, then publishes
  the outbox. A restart can finish publication without applying the fills again.

Keep these existing orders explicit; changing them changes crash recovery.

## Logs

Operational logs are written in English. Service logs should prefer structured fields:

```text
service=<name> event=<event_name> symbol=<...> order_id=<...> reason=<...>
```

Use:

- `DEBUG` for detailed state useful during investigation.
- `INFO` for normal lifecycle/business events.
- `WARN` for recoverable abnormal situations.
- `ERROR` for failed operations that require retry/recovery.
- `ALERT`/fatal-level logging for process-ending failures.

Help text printed to the terminal may use `stdout`; operational failures should use the service
logger where that service has logging initialized.

## How to read one service

For a typical service:

```text
*_main.cpp
    -> CLI options
    -> persistence/recovery helpers (if local to the service)
    -> runtime class
    -> message handlers
    -> service loop
other *.h/*.cpp
    -> focused reusable components owned by that service
```

The main file may still be long when it represents one cohesive state machine. Prefer a
clear single state machine with named sections over scattering one runtime class across many tiny
files. Split a component only when it has a genuinely independent responsibility.

## Readability rules for this directory

- Code, comments, log messages, and technical names are in English.
- Prefer explicit code over clever code.
- Prefer a descriptive 10-line function over a dense 3-line expression.
- Keep real orchestration readable in `main()`; do not introduce forwarding-only application wrappers.
- Comments explain *why* ordering or state matters; they do not narrate obvious syntax.
- Avoid historical `PATCH XX` / `STEP XX` comments in active code. Describe the current rule.
- Do not duplicate trading calculations in services if the domain library already owns them.
- Do not silently mix business time with technical time.
- Do not hide message ordering, durability, or idempotency requirements.
- A new reader should be able to answer: what does this service consume, what does it change,
  what does it persist, and what does it publish?

## Building

Use WSL with the repository Meson build. From the repository root:

```bash
meson compile -C build -j 3
bash validation/live_pre_exchange_e2e_audit.sh
bash validation/market_data_time_handler_audit.sh
bash validation/historical_visibility_no_lookahead_test.sh
```

For a new build directory, run `meson setup <build-dir>` first. Cross-service structural
changes also require the canonical replay release gate:

```bash
bash validation/step59_canonical_replay_release_gate.sh
```

The compact gate proves canonical replay behavior; it does not replace broker/database
integration acceptance for the deployed live services.
