# Current state

Last synchronized: 2026-10-10. Current source and test output take precedence.

## Pending reporting requirements recorded (2026-10-10)

The user requested separate Commission and Funding columns in dashboard Trades,
with USD values and PnL including those costs/receipts; gross trade PnL is optional.
Actual event accounting, attribution and consistent net totals remain prerequisites;
missing costs must stay unavailable and commissions/funding must not be counted
twice. Future user-specified dashboard adjustments are a separate roadmap task.
The roadmap is consolidated to remaining work, preserving accepted local/VPS study
preparation and pending multi-day/private/recovery acceptance. This is a documentation
update only: no reporting/accounting implementation, deployment, restart or study
configuration change. Prior validation evidence below remains unchanged.

## Latest validated baseline

### VPS PAPER activity study and fast comparison deployed (2026-10-10)

The user authorized a VPS forward PAPER study with strict RSI(7) entry > 50 and
exit < 40, plus simulated-live versus fast-backtest results in its dashboard.
The separate activity profile preserves original 80/70 configuration and uses
separate NATS/PostgreSQL volumes; original account/message state is retained.
The comparison collector freezes fully applied/reconciled daily market/account
observations and invokes CURRENT TradingEngine through the existing fast research
binary. It warms indicators only, preserves daily candidate lists and persistent
signals, and observes execution-open prices without unfinished-close leakage.
PAPER close-based quantity versus fast next-open quantity differences remain visible;
cash/equity use common observed open marks. Funding/slippage are not modeled.
GET-only Kraken plan observations remain separate from simulated fills/private funds.

Two bounded WSL baseline tests pass: poisoned future close exclusion, next-open
quantity, commissions, deterministic rerun, date joins and signed exposure. Full
Go tests and frontend image/type build pass. Fresh Step59 PASS: 107 days, 50 fills,
RealTest 25/25, zero differences; all restart/dashboard/pacing paths retain fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/paper_activity_step59.log, paper_comparison_go.log and image logs.
The complete WSL build passes for eight service targets plus the fast research
binary. Three images were exported via docker save, transferred and loaded on the
VPS; no destination C++/frontend build was needed. The bounded PAPER fixture passes
completed-bar isolation, execution-open prices, actual fills/accounting, dashboard
telemetry, stale collector and restart. Initial local attempts found an older API
image and exhausted Docker subnets; selecting matching images and removing only
this turn's stopped fixture networks resolved them. Original local image tags and
all fixture volumes/evidence were preserved. Accepted fixture:
storage/paper_validation/algotrading-paper-7ec7043c76b3/.

VPS acceptance passes with thirteen running services and enabled/active trading,
telemetry and comparison units. Its first observed execution day is 2026-10-10:
two simulated positions (NEARUSDT/QNTUSDT), zero signal differences, PAPER equity
99980.00204792136 USD versus fast equity 99980 USD, and cash 79982.04996928117
versus 79980 USD. The NEAR close/open gap explains the retained quantity difference.
Collector restart preserves the single observed row and economic values, without
resubmitting simulated orders. Kraken public plan observation is retained; no
private Kraken credentials/account/submission was introduced on the VPS.
Authenticated API and one-page browser acceptance verify the 50/40 rules, chart
and comparison tables. The original PAPER account/message volumes remain selected
only by the old profile, with a stopped-state backup under
/opt/algotrading/storage/vps_deployment/activity-before-1791646615/.
Evidence: storage/vps_deployment/activity_acceptance.json and
dashboard_activity_comparison.png/.txt; the VPS retains frozen study input/fast
results under storage/paper_trading/comparison/ and public Kraken reports under
storage/kraken_shadow/algotrading-paper-activity-forward/.
Loopback dashboard remains port 8092; the local SSH tunnel exposes localhost:8093.
Credentials were consumed only internally and never displayed. Multi-day forward
acceptance, funding/collateral/private integration and broader VPS recovery remain
pending. No full-history or broad slow-browser campaign ran.

### Daily portfolio equity percentages; further order-limit work cancelled (2026-10-10)

The user cancelled additional order-limit/admission work and its pending clarification.
Preserve existing order planning, sizing and constraints. Warning-only account
monitoring remains implemented; actual local forward shadow is the next task.

Daily Telegram position lines now include marked absolute USD notional as a percentage
of that snapshot's net venue marginEquity. Yesterday retains yesterday's denominator;
existing historical snapshots are preserved. Missing marks and nonpositive equity
remain unavailable rather than producing invented percentages. Exposure may exceed
100%; this display is not funded margin or a new sizing constraint.

Five WSL portfolio tests and twelve offline monitor tests passed; the optional Docker
contract case was skipped in this targeted run. The local observer was restarted and
its healthy source/notifier retained two events and two receipts without duplicate
delivery. Daily decline remains unavailable until a preceding-day snapshot exists.
Evidence: storage/kraken_shadow/account-alerts/delivery-acceptance.json.
No credentials were displayed, no summary was forcibly resent and the VPS is unchanged.
This presentation-only change did not require another Step59 run; the accepted
fingerprint and previous bounded gate evidence below remain the baseline.

### User-selected warning-only account thresholds implemented locally (2026-10-10)

The user rejected automatic account stops/closures. USD 50 minimum net equity and
10% available USD remain warnings only. The local monitor now also warns when current
net equity is at least 10% below the preceding UTC day's last observed equity, or
gross confirmed open-position USD exposure exceeds 5x current net equity. Exactly
5x does not trigger the exposure warning. There is no independent daily-loss stop
or peak drawdown threshold. Equity comparisons include BTC valuation and transfers;
they are not a trading-only PnL calculation. Pending orders are not filled exposure.

The gross calculation reuses fresh verified public linear USD marks and sums
absolute LONG/SHORT notionals. Daily comparison reuses preserved UTC holdings/equity
snapshots. Missing prior-day history, missing/stale prices or nonpositive denominators
remain unavailable and cannot manufacture a resolution. status.json lists those
unavailableChecks independently of authenticated source freshness. Warning events
share the existing transaction/deduplication and Telegram lifecycle; no new sender,
orders, BTC sale, core Account mutation or stop command was introduced.

Thirteen WSL monitor tests passed including the bounded network-disabled Go contract
case. Five portfolio, 26 account, 13 public Kraken and five shadow tests passed.
New cases verify exact 10% daily boundaries without submission/admission mutation,
daily rather than peak comparison, missing-day retention, exact 5x versus >5x,
and missing-price retention plus diagnostics. The real local observer was restarted
with the finalized code. Its source/notifier are healthy and retain two events and
two receipts (previous reserve warning and today's daily summary), without duplicates.
Daily decline is currently unavailable because no preceding-day snapshot exists.
Evidence: storage/kraken_shadow/account-alerts/ and delivery-acceptance.json.

The user also requests an initial 10% new-order cap. Its total-notional-versus-funded-
margin basis is an essential pending clarification; no new admission cap was invented.
Source verification found existing equal-weight sizing of 10% per full signal and
a 1.5x hard gross strategy scaling constraint. These are preserved and are distinct
from the warning-only 5x actual account exposure. VPS/credentials/history remain
unchanged. Finish the order-basis clarification/guard before actual forward shadow.

Fresh bounded Step59 PASS: 107 days, 50 fills, RealTest 25/25, zero differences;
determinism, resumed/system/dashboard and paced paths converge to fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. No full-history or browser campaign ran.

### Compact Telegram messages and daily USD portfolio accepted locally (2026-10-10)

All shared Telegram formatting now shows importance, account/system, reason and
concise figures. CRITICAL is displayed as URGENT and recovery as INFO. Optional
account labels pass through the source/notification contracts; Kraken defaults to
Kraken. Hash IDs, notification IDs, technical event/correlation metadata and raw
source timestamps stay in durable evidence rather than the chat message. Existing
delivery receipt deduplication is unchanged; normal details are bounded at 600
characters and portfolios use the existing overall Telegram limit.

The read-only account monitor emits one INFO daily summary after the configured UTC
hour (default 09:00), transactionally persisted with its event. Gross USD wallet
balance and net venue margin equity are separate, explicit fields. Linear Multi-M
positions use fresh public mark prices for absolute USD exposure with LONG/SHORT
direction; missing/unsupported/stale prices remain unavailable. Yesterday uses the
last observed snapshot in the preceding UTC calendar day and reports its timestamp;
absent days are unavailable, never invented zero holdings. This is a holdings
comparison, not a complete intraday trading history. No account/strategy/execution
economics, order writes, BTC sales or stops were changed.

Five new portfolio tests, nine monitor tests including network-disabled Go contract
acceptance, 26 account tests, 13 Kraken public tests and five shadow tests passed.
Go go test ./... passed with CGO_ENABLED=0, matching the Docker build. The initial
default-CGO attempt lacked libpq in the test image; this was an environment failure.
The updated local image was built and both local Telegram notifier containers were
replaced while preserving their notification volumes. Three user-authorized,
clearly TEST-labelled format previews were accepted by Telegram; fixtures/receipts:
storage/telegram_previews/telegram-preview-1791641194340836467/.
The real account daily portfolio was also accepted: account-alerts deliveryCount=2
including the previously retained warning, with INFO admission and two durable
receipts. Source heartbeat was healthy; there was no replay of the old warning.
Actual account currently has no open positions; yesterday is unavailable because
daily holdings capture started today. Local monitor remains active; VPS unchanged.
All credential-file consumption was internal to programs/Docker, never displayed.

Fresh bounded Step59 passed: 107 days, 50 fills, RealTest 25/25, zero differences,
all deterministic/resume/dashboard/paced paths converge to fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Permanent observer installation/history
retention and remaining stop-control agreement/actual forward shadow remain pending.

### Local Kraken margin/reserve Telegram monitoring accepted (2026-10-10)

The read-only account monitor publishes durable step43-v1 warning/recovery events
for the existing Go notifier in an isolated local container. It polls every 30
seconds using the existing user-selected USD 50 net equity floor and 10% available
USD policy. Current/stressed USD shortages share one warning; equity-floor and
initial/maintenance margin warnings remain distinct. No order, automatic conversion,
BTC sale, entry stop or core Account update is performed. Failed/stale/invalid reads
cannot manufacture a recovery. An exclusive process lock and policy/account-bound
journal protect producer restart and source identity.

Nine WSL monitor tests passed, including network-disabled TEST_FILE Go warning,
resolution and notifier restart acceptance. Existing 26 account, 13 public Kraken
and five shadow tests also passed. A real authenticated local read produced one
USD_RESERVE warning, accepted by Telegram with one durable delivery receipt.
Repeated reads and observer restart retained one event/receipt.
One subsequent poll failed safely: the notifier paused on the failed heartbeat,
retained the active warning and resumed after a fresh successful observation without
another delivery. The sanitized failure does not establish its upstream cause.
Evidence is under ignored storage/kraken_shadow/account-alerts/: events.jsonl, status.json,
account-observations.sqlite, notifier.json and delivery-acceptance.json. No .env
contents were opened by assistant tools; credential consumption remained internal.

The local observer remains running in WSL with a separate Go notifier. It requires
the PC/WSL/Docker and network; there is no observer reboot installation or VPS change.
Permanent observation-history retention is pending. Remaining loss/connectivity/
reconciliation and entry-stop versus closing behavior precede actual forward shadow.

Fresh bounded Step59 passed on verified current library/canonical source: 107 days,
50 fills, RealTest 25/25, zero differences; all deterministic, resumed, dashboard
and paced paths converge to fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Generated .ai navigation was refreshed
and checked; credential and monitoring artifact paths remain ignored by Git.

### User account floor and relative USD reserve validated (2026-10-10)

The user chose a USD 50 minimum account value and at least 10% available USD.
The explicit implementation basis is reported marginEquity in USD (net usable
collateral/equity), never leveraged position notional. The ignored policy file
storage/kraken_shadow/account-policy.json specifies minimum_account_equity_usd=50,
minimum_usd_reserve_fraction=0.10 and zero additional assumed future cash debit.
Zero assumed debit is not zero trading fees/funding; future cost forecasts remain
pending. Fixed-USD illustrative policies remain supported.

The equity floor and current USD reserve are independent checks. Available USD is
the lesser of reported USD quantity and availability; the requirement is 10% of
positive reported equity. At USD 50 this is USD 5. Equity below the floor flags
ACCOUNT_EQUITY_BELOW_MINIMUM; insufficient current USD flags USD_RESERVE_LOW.
Loss/charge stress is separately labelled USD_RESERVE_LOW_AFTER_LOSS_STRESS,
without subtracting PnL twice from account equity. No allocation, conversion, BTC
sale, order admission or closing behavior is connected to these observations.

Twenty-six WSL account tests plus thirteen Kraken public and five shadow tests
passed. New checks cover boundaries, equity versus leveraged notional, missing USD,
zero/negative equity, invalid policies, separate stress warnings, policy-bound
journal isolation and CLI report export. Real read-only policy evaluation passed
in a separate preserved journal: storage/kraken_shadow/account-live-policy-50-10/
contains report.html, report.json, account-observations.sqlite and last_attempt.json.
The account passes the USD 50 net floor but reports both low-current-USD and
low-stressed-USD warnings, with zero positions/orders. An earlier snapshot attempt
failed safely before observation; the successful fresh read did not relax clocks.
Credentials were consumed internally only, without assistant-visible contents.

Fresh bounded Step59 PASS on verified current canonical source: 107 days, 50 fills,
RealTest 25/25, zero differences and deterministic/restart/dashboard/pacing convergence.
Fingerprint: 94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. VPS/Telegram/core trading remain unchanged.
Remaining loss/connectivity/reconciliation and entry-stop versus close behavior must
be agreed and tested before private orders. These are read-only warnings only.

### Kraken authenticated read-only account inspection accepted (2026-10-10)

The user saved the Futures keys and authorized the account check while requiring
that credential files never be opened/displayed by the assistant. The program
alone consumes storage/kraken_shadow/.env internally; no shell sourcing, environment
dump, credential output or assistant-visible file contents were used. The dedicated
loader accepts the two named variables, optional quotes, UTF-8 BOM/CRLF and comments;
duplicate/missing/invalid entries give constant corrective errors without values.

`python3 live_trading/kraken_account.py --account` now passes a real authenticated
inspection after the initial HTTP 403 was resolved by an explicit application
User-Agent. The permission gate accepted General READ_ONLY/transfer NO_ACCESS.
All five fixed GETs completed and the authoritative multiCollateralMarginAccount
wallet reports BTC/USD currencies, zero positions and zero open orders. This does
not prove a funded BTC allocation, future order acceptance or liquidation readiness.
The check remains REVIEW_REQUIRED solely for USD_RESERVE_POLICY_NOT_CONFIGURED;
no actual reserve/allocation was assumed and no illustrative policy was applied.
No private order, cancel, transfer, settlement change, BTC sale or core Account
update occurred. VPS/Telegram remain unchanged; unrelated working-tree work is preserved.

Safe evidence is retained under ignored storage/kraken_shadow/account-live/:
report.html, report.json, account-observations.sqlite and last_attempt.json.
Only normalized account observations and our status wording persist, never raw
permission replies, keys, signatures or raw account UID. Failed attempts cannot
produce accepted reports. HTTP 401/403 have distinct sanitized diagnostics; 403
alone does not establish invalid credentials. Fixture and actual journals remain separate.

Twenty WSL account tests plus thirteen public Kraken and five existing shadow tests
passed. Fresh bounded Step59 also passed on source-verified current canonical code:
107 days, 50 fills, RealTest 25/25, zero differences and restart/dashboard/pacing
convergence. Fingerprint:
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Gate evidence: storage/pipeline_risk_step59.log.
New coverage includes internal loader formats, CLI/source separation,
User-Agent, HTTP diagnostics and durable failed-attempt status. Actual account
inspection is read-only acceptance, not forward strategy or live-capital acceptance.
Next: agree on BTC/USD reserve and stop policy, then actual forward venue shadow;
funding/conversion/interest economics and supported private lifecycle testing remain pending.

### Kraken read-only account preparation accepted locally (2026-10-10)

The user deferred saving the Kraken key. `live_trading/kraken_account.py` prepares
a separate reader with five fixed GET routes: permissions, wallets, open positions,
open orders and PnL preferences. READ_ONLY/no-transfer permissions are checked
before account reads. Authentication was tested with synthetic in-memory credentials;
no actual keys were read, saved or used. Redirects, proxy inheritance, oversized
responses and stale/non-UTC clocks are rejected. GET authentication omits Kraken's
optional nonce, with no body/query. Errors/reports exclude keys and the raw account UID.

Only the reported Multi-M flex wallet is supported. Venue collateral, marginEquity
and initialMarginWithOrders are authoritative: no fixed haircut is reapplied and
PnL/funding are not added twice. A USD-reserve stress check includes assumed future
cash debits and unrealized losses. Missing USD/policy, margin shortfalls, other
collateral or non-USD settlement require review. Short quantities and partial orders
remain observations, never new fills. Separate SQLite state binds account/source/
environment/policy, rejects conflicting/out-of-order captures and deduplicates after
restart. It never updates lib/Account, submits orders, sells BTC or approves trading.

Fourteen offline WSL account tests, thirteen existing Kraken public tests and five
existing shadow tests passed. Synthetic evidence: storage/kraken_shadow/account-local-20261010/
contains fixture.json, policy.json, report.json, report.html and account-observations.sqlite.
Artifacts are ignored; balances/reserve thresholds are illustrative, not user inputs.
Fresh Step59 PASS on byte-verified canonical source: 107 days, 50 fills, RealTest
25/25, zero differences and restart/dashboard/pacing convergence; fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Gate evidence: storage/pipeline_risk_step59.log. VPS and Telegram remain unchanged.

Actual credential loading, account/product acceptance, a supported private test
environment, agreed reserve/stop policy, forward shadow and derivatives funding/
conversion/interest accounting remain pending. The CLI is explicitly fixture-only;
the account reader is separate from the runtime. Usage: live_trading/README.md.

### Kraken public shadow and mixed BTC/USD preparation accepted (2026-10-10)

The user selected Kraken Multi-M as the initial integration target, with BTC held
as collateral and USD for charges/losses. No account allocation or reserve threshold
was chosen. `live_trading/kraken_shadow.py` now reads fixed public GET instruments/
tickers only, normalizes PF_ linear USD contracts and observes immutable CURRENT
USD notional plans against current marks. Negative precision/scaled source assets,
inactive/inverse exclusion, unavailable books, incomplete metadata and fresh server
clocks are explicit. An empty book is retryable unavailability, not evidence of
an unsupported market. No credentials, submit/cancel routes, wallet transfers,
strategy recalculation, private execution or core-account economics were added.

ShadowJournal accepts a venue assessor with durable source/environment/schema
identity while preserving fixture defaults, source risk hashes, payload conflict
checks and retry/restart ordering. Fixture journals cannot be relabelled as Kraken.
Raw public bytes/GET receipts/hashes, normalized snapshots and source plans persist
per capture so retained observations remain linked to their original evidence.

The live public study passed: 201 PF_ catalogue entries, one exported PAPER fixture
plan, BTC/ETH observations on PF_XBTUSD/PF_ETHUSD, and a restart with exactly one
completed durable plan and two retained raw captures. These are observations of
historical fixture intents, not actual forward strategy or private order acceptance.
The illustrative cross-margin scenario (0.1 BTC, USD 2000, assumed USD 50 cash
charges, unrealized PnL -100 and funding -1, initial/maintenance margin 500/250,
USD reserve 500) passed scenario checks. These numbers are synthetic inputs, not
user balances, recommended allocations or verified venue margin requirements.
The diagnostic uses the BTC index and reviewed 1% haircut, distinguishes USD
reserve from collateral equity, flags uncovered losses/margin shortfalls and
never sells BTC or returns account trading approval. Actual margin schedules,
isolated/unified wallets, other assets and authoritative balances remain pending.

Thirteen focused Kraken WSL tests plus five existing shadow unit tests passed.
The full saved-response audit passed: raw hashes, GET routes, catalogue identity,
original plan/snapshot hashes, quantities/read-only state and scenario arithmetic.
Evidence: storage/kraken_shadow/local-20261010-btc-usd/{report.html,report.json,
observations.sqlite,raw/}; hypothetical input: storage/kraken_shadow/example-collateral.json.
All artifacts are ignored; no VPS or Telegram changes. Existing unrelated work was
preserved. Fresh WSL Step59 PASS on byte-verified current native canonical source:
107 days, 50 fills, RealTest 25/25, zero differences, deterministic/restart/dashboard/
pacing convergence; fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log.

The legacy demo public instruments probe failed on a forbidden redirect; no live
fallback occurred. Kraken's current [testing guide](https://support.kraken.com/articles/360024809011-api-testing-environment-derivatives)
also announces decommissioning of that environment; confirm an available supported
test environment before private lifecycle tests. The user confirmed their account
country is Spain and Futures is enabled (2026-10-10). This resolves the requested
country/activation clarification; wallet type, BTC eligibility on that wallet,
USD profit settlement and API access still require actual account evidence.
Next: validate the prepared read-only wallet/margin inputs with a Derivatives key
(General API Read Only; Withdrawal API No Access) and agree on reserve/stop policy
before private test integration. No private credentials or API checks are available
yet; user-reported activation is not an authenticated account acceptance.
BTC/USD can reduce conversions while USD covers charges/losses; trading fees,
funding and uncovered-loss interest remain economic requirements. Official EEA
collateral/charges/eligibility sources and usage are linked in live_trading/README.md.

### Public exchange coverage accepted (2026-10-10)

`tools/exchange_coverage.py` captured public market metadata and daily candles at
11:07-11:08 UTC. Binance server time bounded 25 completed UTC days: September 15
through October 9 inclusive. The reference is currently TRADING crypto USDT
perpetuals, not Binance spot or all quote currencies; 523 candidates were inspected.
Ranking sums actual candle quote turnover (field 7), never base units or close
price estimates. All top-50 assets had 25 observed candles. Current-listing
survivorship bias is explicit; this is current coverage, not a historical universe.

Kraken covers 38/50 (76%), Hyperliquid main covers 36/50 (72%), and Hyperliquid with
inspected HIP-3 markets covers 37/50 (74%). These assets represent respectively
95.6911%, 94.0169% and 95.2846% of Binance top-50 turnover, not venue liquidity.
Kraken covers all top 20 with Multi-M PF_ contracts; Hyperliquid main covers 19,
and HIP-3 xyz:QNT supplies the twentieth. MOVR/MUBARAK are Kraken-only and LIT is
Hyperliquid-only among the compared top 50. Active/nonexpired/nonsuspended metadata
and positive mark prices are required. Explicit scaled-contract aliases retain
1INCH and distinct NEIRO/NEIROETH; identity matches do not prove sizing equivalence.

Evidence: storage/exchange_coverage/20261010T110714Z/{comparison.json,coverage.csv,
report.html,raw/}. Raw responses retain retrieval time, endpoint/request and body
hash. Five focused WSL tests passed; the offline snapshot audit independently
recomputed every candidate's candles, complete ranking, live filters, aliases,
per-asset venue matches and all three counts. Generated evidence is ignored.
No trading source, credentials, private API, VPS deployment or Telegram sends changed;
the existing Step59 baseline below is retained without rerunning an economic gate
for this independent public-data study.

Public collateral rules were checked against current official documentation:
[Kraken Multi-M](https://support.kraken.com/articles/4843323030164-derivatives-collateral-currencies)
accepts BTC with 1% haircut and 0.20% conversion fee; uncovered USD losses can have
additional interest/conversion effects.
[Hyperliquid portfolio margin](https://hyperliquid.gitbook.io/hyperliquid-docs/trading/portfolio-margin)
lists BTC at 0.5 LTV, requiring master-account weighted volume above USD 5 million
or account value above USD 10000, with account value below USD 25 million and
supply/borrow caps. This is distinct from ordinary USDC margin and borrowing has
costs. Public rules do not establish this user's jurisdiction/account eligibility.
Kraken remains the technical preference from coverage and the user's BTC/account
requirements; confirm product/account eligibility and collateral economics before
venue selection, real read-only integration and forward shadow acceptance.

### Local shadow-trading preparation accepted (2026-10-09)

The one-command WSL `python3 validation/shadow_trading_test.py` study passed with
current packaged runtime/API/web images and isolated PAPER state. The existing
service chain produced 200 completed BTC/ETH bars, exactly two BUY intents of USD
10000 at close(T)=199 and two simulated next-open fills at 200. Worker restart
retained fills, cash/positions and the persisted risk evaluation. The shadow hook
read the actual planner checkpoint and retained its request/plan without changing
the original durable plan. No new strategy, planner or fill simulator was introduced.

`live_trading/shadow_observer.py` owns only read-only observation and its separate
SQLite journal. It binds the source risk-configuration fingerprint and immutable
message/payload hashes, rejects conflicts/out-of-order input and reuses already
completed observations after duplicate delivery/restart. Failed fixture reads remain
retryable without becoming successful empty data or unsupported-market skips.
The reader allows only numeric loopback HTTP GET /snapshot, disables proxies and
rejects redirects; it has no private submission/cancellation interface or credentials.

The integrated fixture passed nine grouped checks: real producer quantities use
close(T), duplicate/observer restart, unsupported perpetual skip, HTTP/disconnect/
redirect recovery, stale/future/incomplete/missing metadata, minimum quantity,
quiet/cancel intents, identity/configuration conflict and the HTTP spy. It retained
12 scenario plans and seven unavailable attempts; 19 HTTP calls were all GET
/snapshot, with zero private writes. Synthetic error-case rows are explicitly
labelled and are not additional strategy decisions. Five focused WSL regression
tests also passed, including malformed top-level metadata, failed-attempt identity
conflict, future decision/side mismatch and out-of-order rejection before HTTP.
The final observer was rechecked with the actual saved producer export in fresh
fixture state after the final error-guard/report changes.

Evidence: storage/paper_validation/algotrading-paper-958436ce42ee/accepted.json and
shadow/{producer-input.json,accepted.json,report.json,report.html,venue-calls.json,
observations.sqlite}; shadow/current-source-recheck/ retains the final component run.
Reports and disposable credentials are ignored. Owned containers/networks were
removed; volumes/evidence were preserved and the normal Telegram notifier remained
running. No VPS changes, private venue calls, real Telegram sends or full-history/
browser campaign. Source trading behavior is unchanged.

Fresh required WSL Step59 PASS: 107 days, 50 fills, RealTest 25/25, zero differences,
continuous/resumed/system/dashboard/pacing convergence and fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log, using byte-verified current native source.

This accepts local fixture preparation, not actual selected-venue shadow trading.
The risk fingerprint is not a complete strategy/market/venue configuration manifest.
Full aligned backtest comparison, real venue mapping/rules/price freshness, fees,
funding, FX/BTC collateral/account eligibility and private order/fill/reconciliation
acceptance remain pending. Reports leave costs, fills and PnL unavailable rather
than inventing them. Next prerequisite: reproducible venue coverage/collateral
analysis and selection before actual selected-venue forward observation.

### Mobile-access cancellation and next local task (2026-10-09)

The user cancelled mobile dashboard access and removed it from the roadmap.
No phone login acceptance, domain, VPN or public web deployment is pending.
The unused local bridge/tests and their past validation evidence are preserved;
they are not active operations or prerequisites for trading work. No VPS change
or network exposure was made for this cancellation.

Next is preparing bounded local shadow-trading tests. The roadmap now defines
the no-private-submit/cancel boundary, isolated virtual state, selected-venue
read-only evidence, unsupported-market skips, aligned baseline comparison and
disconnect/duplicate/restart criteria. This is an acceptance plan, not a completed
harness or venue run. Source inspection confirms the existing gateway has backend
and Hyperliquid dry-run modes; the latter logs raw quantities with
venue_rules_applied=false and submitted=false. PAPER still uses a simulated backend.
Coverage, BTC-collateral/account checks and venue selection remain prerequisites
for actual selected-venue acceptance. Existing economic/replay evidence is unchanged.

### Historical mobile-access preparation, subsequently cancelled (2026-10-09)

`deploy/paper_trading/mobile_access.py` provides an opt-in TLS 1.2+ bridge from one
exact phone IPv4 address to an already-running loopback dashboard or VPS SSH tunnel.
It refuses public/wildcard listener addresses, checks certificate validity/IP,
streams bytes without whole-response buffering and does not log requests/secrets.
Generated certificates/keys are excluded under `storage/mobile_access/`.
The source API still owns authentication, role checks, CSRF and session revocation.

WSL validation passed six bounded transport checks plus actual Go API authentication
through TLS: unauthenticated 401, viewer login, Overview/Pipeline/Risk/Infrastructure
HTTP 200, viewer manual-preview rejection, missing-CSRF logout rejection, successful
logout and revoked-session 401. The isolated packaged API used embedded mock data
and disposable fixture credentials; no trading workers or Telegram sends were used.
Its container was removed. Evidence: storage/mobile_access/api-validation/test.log
and accepted.json. This tests real API authentication, not real market-data readiness.
All six transport tests also passed on Windows with the installed Git OpenSSL;
the optional Go-fixture test was skipped there. Python CLI help and whitespace checks
passed. No full-history or browser campaign was rerun.

Mobile setup/runbook was prepared; actual handset acceptance was not performed
and the user subsequently cancelled this requirement. The observed PC Wi-Fi
IPv4 is 192.168.1.33. No mobile listener/firewall rule or SSH tunnel was left running;
normal services and the VPS were preserved. Remote/mobile-data access is separate.

Fresh WSL Step59 PASS using the byte-verified current native trading source: 107
days, 50 fills, RealTest 25/25, zero differences, continuous/resumed/dashboard/paced
equivalence and fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Trading source/economics unchanged.

### Local PAPER backup, restoration and connectivity acceptance (2026-10-08)

The opt-in PAPER_RECOVERY_TEST study passed using the current packaged runtime/API/
web images and isolated BTC/ETH public-data fixture. HTTP source loss failed ingestion
without committing bars/fills. NATS loss retained 200 already committed completed
bars but generated no fills until publication recovered, matching commit-before-
notification source ordering. Two actual simulated fills completed with all four
dashboard services stopped; cash/positions agreed with the simulated backend.

PostgreSQL outage produced explicit HTTP 503 from Risk rather than successful empty
data. After controlled worker restart and database recovery, retained fills, cash,
positions, processed fill IDs, next order/fill IDs and economic dates were unchanged.
This is explicit restart recovery, not unattended reconnect acceptance.

With all writers/broker stopped and the exchange outbox drained, a PostgreSQL custom
logical dump plus six stopped-volume archives were created. Checksums were verified
before restore; a deliberately wrong checksum was rejected. A separate fresh-volume
project restored identical contents for all 15 public SQL tables and every archived
file before workers ran. Market SQLite includes the stopped database and WAL/SHM;
NATS and watchdog/notifier/audit/acknowledgement stores are included. Manual-audit and
acknowledgement stores were empty in this fixture; their operator event lifecycles
are not accepted by copying empty volumes. Credentials remained separate/ignored.

Restored ingestion completed successfully, all trading workers stayed running and
same-day replay retained exactly two fills, cash 79879.39698492462, BTC/ETH quantities
50.25125628140704 each, next order/fill IDs 3 and zero unpublished exchange outbox
rows. Authenticated restored Risk preserved its configuration fingerprint/evaluations.
No duplicate orders/fills or accounting drift were observed in the bounded study.

Evidence: storage/paper_validation/algotrading-paper-5c503da259d6/recovery-accepted.json,
backup/manifest.json, baseline accepted.json and storage/paper_recovery.log.
Original/restored test containers/networks were removed; their data volumes and
backup evidence remain ignored. The existing normal local Telegram notifier was
preserved. VPS unchanged; no Telegram sends, private orders, full-history replay or
browser campaign. The backup is a quiesced maintenance snapshot with matching
images/settings, not online cross-store backup or off-host/retention/VPS acceptance.
The then-next mobile-access task was cancelled on 2026-10-09; see the latest priority above.

Fresh WSL Step59 PASS: 107 days, 50 fills, RealTest 25/25, zero differences and
fingerprint 94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
including rerun/restart/dashboard/pacing checks. Evidence: storage/pipeline_risk_step59.log.
Python syntax and Git whitespace checks PASS. Trading source/economics unchanged.

### Authenticated local dashboard browser acceptance (2026-10-08)

A bounded isolated PAPER study passed with 200 completed BTC/ETH bars, two
simulated fills, consistent account cash/positions and restart without duplicate
fills. Current packaged runtime/API and a newly built API-mode frontend were used.
The fixture forces TEST_FILE notifications and clears Telegram credentials; its
temporary viewer credentials and all generated evidence are ignored by Git.

Windows Edge acceptance passed for all eleven authenticated pages, invalid/valid
login, sidebar navigation, full document navigation, all six Overview filters with
explicitly unavailable real equity history, Pipeline expansion/Why/search,
persisted Risk rows/configuration provenance, stale Infrastructure telemetry,
viewer-disabled manual upload and HTTP 403 mutation rejection. Browser-only
blocking of the Risk resource showed an error/Retry boundary; unblocking and Retry
restored the report. Logout protected subsequent page access. No page produced a
captured browser runtime error during normal acceptance. This does not accept
operator routing, actual backend connectivity faults, phone access or VPS behavior.

The browser test exposed accumulated SSE connections during repeated full document
navigation: later REST requests timed out without reaching the API. The stream hook
now closes EventSource on pagehide and reconnects on pageshow, as well as closing
on React unmount. The same eleven-page navigation sequence passed after this change.
No trading economics, API data contracts or private routing changed.

Evidence: storage/paper_validation/algotrading-paper-a5d64a25c8dd/accepted.json,
browser/accepted.json and page screenshots; build output is
storage/dashboard_browser_build.log. Only this task's fixture containers/networks
were removed; their data volumes/evidence remain. The normal local Telegram notifier
is still running/waiting for a fresh watchdog; trading services remain stopped.
VPS unchanged. Next: isolated backup/restore/connectivity acceptance, including
continued trading while the dashboard is stopped, then mobile browser access.

Fresh Step59 under WSL PASS: 107 days, 50 fills, RealTest 25/25, zero differences,
deterministic rerun, system/dashboard restart and paced-speed invariance, fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2.
Evidence: storage/pipeline_risk_step59.log. Frontend TypeScript/Vite build, Python
syntax checks, Git whitespace check and dashboard source audit (33 markers) PASS.
The generated AI index was refreshed. No full-history replay, VPS change or commit.

### Historical comparison review accepted by the user (2026-10-08)

The user considers the four full-history differences explained and closes their
review: final-position closing conventions and intermediate RealTest RSI rounding.
This is user acceptance of the recorded differences, not a new exact-parity proof.
The original comparison CSVs, four DIFFERENT rows and full-history fingerprint are
unchanged. No whitelist or trading/economic adjustment was introduced, and no
full-history replay was rerun. Dashboard acceptance is recorded above.

### Continuous local Telegram delivery and recovery (2026-10-08)

The packaged notifier passed a real, explicitly authorized two-message controlled
test: a synthetic WARN OPENED alert and its RESOLVED transition, clearly labelled
TEST ONLY. Its source was an isolated watchdog-format journal, not a trading failure.
Disconnecting its Docker network left the event unprocessed with no delivery;
reconnection delivered it. Two restarts retained delivery decisions/receipts without
resending. An expired source heartbeat blocked consumption; fresh source recovery
resumed polling. Exactly two receipts remained. The test container and its own state
volume were removed; source/evidence remain ignored under
storage/telegram_validation/algotrading-telegram-6a374aeb866e/.

The notifier now requires a successful watchdog heartbeat within 90 seconds,
rejects timestamps over 30 seconds ahead and waits after a failed watchdog sweep.
This avoids replaying obsolete alerts while the local stack is stopped. PAPER
healthchecks reject a recorded lastError rather than treating an old success as
current health. Full Go suite and current API/notifier image build PASS; focused
tests cover healthy/stale/future/missing/error source states.

The normal local dashboard-alert-notifier is running with restart=unless-stopped
and the validated algotrading-telegram-api image, selected by ignored
storage/paper_trading/telegram.override.yml. All trading services remain stopped;
the notifier therefore waits for a fresh watchdog, not trading signals. Its state
and Telegram receipts use the normal durable PAPER volume, separate from TEST_FILE.
Evidence includes storage/telegram_continuous_go.log,
storage/telegram_continuous_build.log and
storage/telegram_validation/continuous_running.json. VPS unchanged; no real orders.
The current notifier healthcheck correctly reports unhealthy while its watchdog
source is absent; process running is separate from delivery readiness. Dashboard
contract audit PASS (33 markers), source clock audit PASS and Step59 PASS with
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills and RealTest 25/25 with zero differences. Gate evidence is
storage/pipeline_risk_step59.log. No full-history or browser campaign was rerun.

### Real Telegram setup acceptance (2026-10-08)

The user configured the local bot and sent a fresh private message. Its private chat
was resolved through the bot API and saved only in ignored storage/paper_trading/.env;
the configured chat was verified before the explicit packaged --test-message command.
Telegram accepted the setup message and the notifier persisted its durable receipt.
An initial sanitized transport failure was followed by a successful retry after
checking public HTTPS access and the runtime CA bundle. No credentials or private
message contents were printed. API acceptance is observed and the user confirmed
that the setup message appeared on their phone. Continuous watchdog-event delivery
is separate from this confirmed setup test.

Evidence: ignored storage/telegram_validation/real_send/accepted.json and send.log.
Continuous alert delivery was not started, no trading stack was started, and the VPS
was not modified. Actual watchdog-event delivery/recovery remains separate acceptance;
the four historical replay differences remain pending. No code/economic change or
release-gate rerun was needed for this credential/configuration setup.

### Local Telegram preparation and operational alerts (2026-10-08)

PAPER no longer hardcodes TEST_FILE: Telegram is an explicit sink setting, with
token/chat ID forwarded only to the notifier and TEST_FILE preserved as default.
The notifier supports --test-message for one manually invoked setup message, without
replaying watchdog alerts or changing trading state. The existing sink now respects
Telegram retry_after across all alerts while running; rejected events remain
unprocessed and retry after the deadline. Confirmed delivery receipts persist and
prevent replay after a missing notifier decision checkpoint. The in-memory throttle
resets on restart; a crash after remote acceptance but before local receipt write
can still duplicate delivery. No exactly-once remote-delivery claim is made.

Fresh host resource pressure, observed missing/stopped trading executables,
unsynchronized NTP and degraded daily PAPER progress now enter the existing alert
projection/watchdog lifecycle. UNKNOWN/stale telemetry is not a measured process or
clock failure; no-order daily success is not an outage. Existing notification
severity/cooldown/escalation/resolution policies are preserved.

WSL full Go suite PASS, including simulated rate limiting, later retry, cross-alert
deferral, durable receipt recovery, operational alert derivation and recovery;
current API/notifier Docker image build PASS. Isolated Compose rendering PASS for
default TEST_FILE and opt-in Telegram credential forwarding using fixture values.
Packaged --test-message rejects TEST_FILE with networking disabled. Evidence is
ignored storage/telegram_go.log, storage/telegram_api_build.log and
storage/telegram_compose.log. No real Telegram message was sent: user bot/chat
settings and real delivery remain pending. No VPS change, full-history run or commit.
The four historical replay differences remain a separate pending review.
Dashboard contract audit PASS (33 markers), no-legacy-clock source audit PASS;
Step59 PASS with the expected fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25 and zero differences across restart/dashboard/
pacing paths. Fresh gate evidence is storage/pipeline_risk_step59.log.

### Local runtime-owned Risk evaluations (2026-10-08)

PortfolioRisk optionally captures actual sized weights, asset/gross cap reductions,
gross scaling, annualized volatility diagnostics and rebalance actions. The service
stores a schema-versioned report with cycle/configuration identity atomically in
the existing decision checkpoint. Control messages and publish/checkpoint/ACK order
are unchanged. Runtime retries reject changed configuration identities. Risk shows
the persisted intermediates, reduction reasons, held quantity and configuration
SHA-256. HOLD preserves quantity and does not emit a newly evaluated target.

Missing sizing or volatility is explicitly unavailable; EqualWeight volatility is
not applicable. Optional volatility diagnostic failure does not fail valid trading.
Old schemas/rows and canonical replay snapshots retain unavailable evaluations.
Whole-account/venue margin breaches, marked real PnL and control readiness remain
pending; per-strategy target caps do not prove held exposure satisfies those limits.

WSL current-source native build PASS; focused C++ signed/capped/volatility/HOLD tests
PASS; full Go suite and API/frontend TypeScript/Vite Docker builds PASS. Authenticated
PostgreSQL/HTTP fixture PASS for old schema, same-cycle reports, rejected mismatched
reports and unchanged cash. Actual bounded eight-service PAPER fixture PASS: two
simulated fills, correct Risk evaluation, identical reports/fills/accounting after
restart. Test project algotrading-paper-c3470b99bc3b retained ignored evidence.
Step59 PASS: fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25, zero differences. Evidence is ignored
storage/risk_evaluations_*.log and storage/pipeline_risk_step59.log.
The first fixture startup hit exhausted local Docker network pools; only two
verified empty networks belonging to a stopped earlier fixture were removed before
retry. Its data volumes and other local projects were preserved. No full-history
replay, slow browser campaign, VPS change or commit was performed.

### Local Infrastructure observations (2026-10-08)

Infrastructure now separates container state from externally observed trading
executables. The host collector uses bounded Docker top queries with PID/name/state,
never process arguments, and distinguishes running, stopped/defunct, missing and
unavailable executable observations. Services retain Ready=false; presence does not
establish application responsiveness, an in-loop heartbeat or private readiness.
Stale snapshots discard process and clock observations with host/container metrics.

Daily PAPER progress observes persisted last_bar_close_timestamp and
last_execution_timestamp: yesterday's completed-day decision and today's applied
plan, including empty/no-order plans. UTC rollover has an explicit 30-minute grace.
Recent updated_at/fills cannot conceal stalled dates; other business-clock modes
are not compared against today's date. Plan application is not fill/reconciliation
acceptance. The UI labels host uptime, process state and container observations
explicitly, with more readable operational text.

Host NTP synchronization is read from systemd NTPSynchronized, independently of the
dashboard container. SYNCED/UNSYNCED require observed evidence; unsupported/unavailable
queries remain UNKNOWN and offset is unmeasured. Actual local WSL query returned
unavailable, so synchronized-host/VPS acceptance is still pending. Shell warnings
and global clock requirements reflect observed evidence without enabling routing.

Validation: WSL collector unit tests and an isolated real Docker process fixture
PASS (running, stopped inside a running container, absent, stopped container);
full Go suite PASS; final API/frontend Docker builds and TypeScript/Vite PASS.
Packaged authenticated PostgreSQL/HTTP fixture PASS for current no-order progress,
missing process, unsynchronized clock, stalled business dates and stale telemetry;
account cash remained unchanged. Step59 PASS: expected compact fingerprint
94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2,
107 days, 50 fills, RealTest 25/25, zero differences. Source audit and AI index PASS.
Evidence includes ignored storage/infrastructure_go.log,
storage/infrastructure_sql_http.log and storage/pipeline_risk_step59.log.
No full-history replay/browser campaign, VPS deployment, commit or trading-engine
change was performed. Test containers were removed; production PAPER remains unchanged.

### Local Pipeline/Risk read-model repair (2026-10-08)

Source tracing identified two empty-screen causes: service HOLD/no-new-target
instructions omit assets from DecisionBatch, and the simulation provider returned
empty Pipeline rows and risk-limit lists by construction. The real pipeline now
includes observed planner reference assets, explains truly empty asset lineage,
and distinguishes missing planner/first-decision checkpoints from no-order success.
Absent signals are UNKNOWN; absent targets remain HOLD rather than displayed zero
liquidation instructions. Database failures and mismatched timestamps remain errors.

Real Risk now reads allocation, sizer settings, gross/asset caps and rebalance policy
from the persisted service configuration identity. These are settings, not evaluated
breach/clipping evidence. Empty emitted decisions retain account cash and an explicit
no-new-target explanation. Combined approved asset weights use aggregated notionals
and emitted capital, fixing the addition of differently allocated strategy percentages.
Replay Pipeline now shows market/position/retained-order observations with expandable
stages and Why traces; cumulative earlier fills do not become current-cycle proof.
Replay Risk shows actual marked account positions without an invented armed kill
switch, policy limit, strategy signal or breach-free verdict.

The WSL Go suite passed, including quiet/HOLD, explicit FLAT versus absent signal,
missing planner, missing decision versus database failure, policy parsing,
multi-strategy capital weighting and replay observation cases. The packaged real API
passed an isolated local PostgreSQL/HTTP fixture: persisted policy identity, quiet
cycle, missing planner pending, mismatched account timestamp rejected, first-decision
pending and unchanged account cash. Its own containers/anonymous volumes were cleaned.
A bounded authenticated Edge check against a copied existing canonical replay
snapshot passed Pipeline rows, expanded stages/Why and Risk account/position rendering.
This reused retained replay evidence; no full-history study was rerun.

Step59 passed in WSL against a byte-verified native copy of current supported trading
source, with the unchanged compact fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`.
TypeScript/Vite builds passed. Evidence is under ignored `storage/pipeline_risk_*`;
the authenticated replay preview was subsequently stopped and removed at the user's
request. The VPS was not contacted or modified.

The later local Risk evaluation work above supersedes the intermediate sizing,
volatility and configuration-persistence gaps recorded at this repair. Whole-account/
venue breaches and current REAL marked exposure/PnL remain unavailable. This repair
alone does not establish full risk telemetry or private/live readiness.

### Local Overview charts and date filters (2026-10-08)

Implemented numeric USD equity/reference charts with dated axes, hover values,
independent X/Y zoom, two-axis pointer drag and reset. Working 1D/7D/30D/90D/1Y/ALL
filters select source observations by UTC dates, anchored at each series' latest
date. ALL shows all received history; 1Y is 365 days. A single daily observation
renders as a point. Filters affect the charts only, not current account snapshots.
Real/PAPER missing marked-equity history remains unavailable.

The WSL Docker TypeScript/Vite build and focused date-window tests passed:
all six ranges, sparse dates, compact canonical date labels, invalid/nonfinite
rows, and empty/single-point histories. A bounded local Edge browser check passed
all six buttons, hover USD readings, independent axis zoom, real pointer drag,
reset and navigation reset after switching timeframes. It used frontend-local
mock fixtures, not production trading evidence. Evidence is in ignored
`storage/overview_build.log`, `storage/overview_windows_test.log` and
`storage/overview_browser/overview.png`. A loopback-only demo preview runs at
`http://localhost:8094`; it is separate from PAPER and the VPS tunnel on 8093.

The VPS was not contacted or changed. No trading/API/accounting behavior changed;
Step59 was not rerun for this frontend-only work. The existing accepted economic
fingerprint remains the baseline. Pipeline/Risk is the next local roadmap task.

### PAPER quote-turnover migration accepted (2026-10-08)

The VPS PAPER deployment now uses `pure_rsi_quote_volume.json`: top 20 by the
25-completed-day average of Binance's actual USDT quote turnover within the
current 24-hour top-50 candidate set. Base-unit volume remains available; neither
strategy nor dashboard approximates turnover with volume multiplied by close.
The original raw-volume canonical profile remains unchanged.

A checked cutover backed up PostgreSQL, market SQLite and configuration before
changing only the strategy's liquidity source. The completed 2026-10-07
strategy/risk/planner checkpoints were preserved. The first new-metric decision
uses the completed 2026-10-08 candle, available on 2026-10-09 UTC. No completed
cycle was rerun, and no account, order, fill or consumer state was reset.

Post-deployment verification independently compared dashboard top-20 ordering
with SQLite quote-volume averages. The leading assets were BTCUSDT, ETHUSDT,
ZECUSDT, SOLUSDT and XRPUSDT; BTC averaged 11.792 billion USDT/day and ETH 8.844
billion. All 13 services were running, with 100,000 virtual cash, zero fills and
no pending backend outbox. The dashboard labels `SMA Quote Volume 25 (USDT)`;
old-cycle signals remain explicitly unaligned until the new decision. The rolling
comparison excludes old-metric cycles and correctly reports INSUFFICIENT_DATA.

Validation passed in WSL: quote-volume C++ tests, the Go suite, dashboard build
and Step59. VPS Step59 also passed with the unchanged compact fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`.
The actual packaged VPS fixture `algotrading-paper-2eb7a44cfb1f` passed completed
quote-data/future-candle isolation, two simulated fills with reconciled accounting,
dashboard telemetry/staleness and restart without duplicate fills. Its separate
containers were stopped; production trading state was not used for the study.

Packaging validation caught stale incremental C++ objects after archive extraction
had assigned zero timestamps. All eight runtime targets were rebuilt from a clean
build before acceptance, and source archives now preserve a current snapshot
timestamp. The fixture's optional cached data-server binary requires identical
source and an ELF header; actual trading services use newly built images.

Evidence lives in ignored `storage/vps_deployment/quote_*` files and the fixture's
`storage/paper_validation/` directory. Backups and previous image tags are retained.
This acceptance does not establish a historical quote-volume performance baseline,
private-exchange readiness or sustained VPS resource capacity. Two-day, 50-MiB
daily log retention remains configured; actual UTC rollover is still pending.

### Lightsail current-data PAPER deployment (2026-10-08)

Deployed the current uncommitted source snapshot to `/opt/algotrading` on the
user's Paris Lightsail VPS: x86_64, Ubuntu 24.04.4 LTS, 2 vCPU, 1,906 MiB RAM
and a 57.1-GiB root filesystem. Added 2 GiB swap as a transient reserve. No local
credentials, mutable databases, Docker volumes or research collections were copied.
The normalized SHA-256 transfer manifest verified 773 initial source/input files;
The final synchronized manifest verifies 774 files, with the installed PAPER/logging
units and AppArmor snippet matching source after the deployment fixes.
The original canonical CSV, RealTest reference and symbol map are included.

All eight PAPER runtime targets built on the VPS and passed container `--help`
packaging checks. The configured project has 20 targets; this deployment did not
build every research target. WSL and VPS Step59 both passed the compact 107-day
gate: 25/25 matched trades, zero differences, 50 canonical fills and fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`, including
determinism, checkpoint/resume, dashboard equality and pacing. Toolchains differ:
local GCC 11.4 / Arrow 23 versus destination GCC 13 / Arrow 25; the accepted bounded
outputs agree, not the compiler or binary hashes.

The real public-data deployment has 13 running services, 4,932 completed bars for
50 coins through 2026-10-07, aligned strategy/risk/planner checkpoints, no fills,
100,000 virtual cash, empty positions and no pending backend outbox. Host reboot
restores durable state and services; receiver restart preserves the same 13 container
IDs/start times. The API and a short authenticated browser check passed. Sampled
host RAM was roughly 640-725 MiB, and disk about 9.6 / 57.1 GiB with 47.5 GiB free
after retaining the fixture/build images;
these samples do not establish sustained capacity, peaks or complete-backtest usage.
The host reports synchronized NTP, while dashboard clock measurement remains UNKNOWN.
Existing global private-readiness/reconciliation badges remain unverified/pending.

Deployment fixes preserve trading logic:

- A VPS overlay gives NATS/PostgreSQL/API restart policies, and a systemd unit
  starts PAPER after Docker and the loopback log receiver.
- Scoped AppArmor rules permit the dedicated receiver and bounded Python writer;
  confinement stays enabled. Listener readiness is checked before dependent startup.
- PAPER wants the receiver rather than depending on its continued lifecycle, so
  restarting diagnostics does not stop trading.
- Dashboard API/watchdog mount market SQLite read-only. Writable observer mounts
  had created WAL/SHM as UID 70 and blocked UID 10001 ingestion after reboot.
  Existing auxiliary-file ownership is restored by the volume initializer, and
  read-only observers cannot recreate the conflicting writable files.

The two-day/50-MiB daily receiver and minute cleanup timer are installed/enabled.
Fourteen service/init identities have emitted daily files; the quiet volume
initializer need not emit one. Unique markers survive receiver restart, cleanup
runs successfully and all observed files remain below the cap. Real UTC rollover,
actual production trimming, prolonged outage and multi-day growth remain pending.
Eight local daily-log tests passed. The final isolated PAPER fixture passed on the
VPS (`algotrading-paper-ec80b2f4b53b`): completed-bar visibility, execution-only
opening prices, two fills with reconciled accounting, dashboard telemetry/staleness
and restart without duplicate fills. It uses separate state and stopped its own
containers. A local rerun could not allocate networks because Docker's default
address pools were exhausted; no unrelated networks/volumes were removed.

Evidence is under ignored `storage/vps_deployment/` locally and on the VPS, plus
the VPS fixture's `storage/paper_validation/algotrading-paper-ec80b2f4b53b/`.
Viewer credentials are fresh on the VPS; the ignored local viewer-only copy is
`storage/vps_deployment/.env`. The private key stays outside the repository.
Remote viewing uses an SSH tunnel on localhost:8093; local PAPER remains on 8092.
The VPS runs independently of the PC, while remote viewing needs the client tunnel.
No private venue, real funds or Telegram delivery is enabled. Next observe successive
daily cycles/UTC log rollover, then perform the remaining recovery/capacity campaign.

### Current public-data paper session running (2026-10-08)

Started the prepared `algotrading-paper` project locally with its separate durable
volumes and left all 13 long-running services plus the host/log collector running.
The real public Binance download produced 4,932 completed rows for 50 coins through
2026-10-07; available history begins 2026-06-30. Strategy, risk and planner each have
the aligned 20261007 checkpoint. Execution completed the 20261008 no-order cycle:
zero active signals/fills, 100,000 virtual cash, empty positions and no pending
backend outbox. Execution/backend cash and positions agree. This is an actual
current-data no-order cycle, distinct from the earlier two-fill synthetic fixture.

Authenticated Infrastructure API and a short headless Edge check passed: PAPER /
VIRTUAL FUNDS, CPU/RAM/disk amounts, fresh observation timestamp and service rows
render. All 13 containers are RUNNING; containers without Docker health checks
report WARN rather than claiming business readiness. Disk WARN reflects the local
drive (about 91% used, 41.5 GiB free), not a VPS measurement. A single container
snapshot totaled 167.37 MiB RAM and 5.65% CPU of one core; these are sampled values,
not sustained averages/peaks or a VPS capacity acceptance. Host totals separately
include WSL activity. Existing global live-readiness/reconciliation badges still
show unverified/pending evidence; this session does not accept private live routing.

Ignored evidence: `storage/paper_trading/current_data_acceptance.json`,
`startup_diagnostics.log`, `dashboard_infrastructure.png` and
`dashboard_infrastructure_text.txt`. The background monitor PID is stored in
`storage/paper_trading/monitor.pid`; the generated local stop helper checks process
identity and preserves volumes:

```powershell
wsl --exec python3 storage/paper_trading/stop_local.py
```

Open http://localhost:8092 as `viewer` with the password from ignored
`storage/paper_trading/.env`. Keep Docker/WSL and the PC awake for daily operation.
No trading source/configuration was changed or release gate rerun for this startup;
the accepted Step59 evidence below remains current. VPS creation, deployment,
multi-day operation and deferred fault/retention acceptance remain pending.

### Current-data paper preparation and dashboard telemetry (2026-10-08)

The separate `deploy/paper_trading` stack builds under WSL with current ingestion,
100-day PureRSI warmup, virtual cash, the existing simulated backend and real-source
dashboard. No VPS deployment or private exchange integration was performed.
The bounded fixture `validation/paper_trading_test.py` passed in project
`algotrading-paper-4d6e89bb5e20`; retained evidence is
`storage/paper_validation/algotrading-paper-4d6e89bb5e20/accepted.json` and
`storage/paper_integration_second.log`. It verified:

- Missing opening prices fail before completed-bar commit.
- Exactly 200 completed rows through 2026-10-07; poisoned unfinished current-day
  high/low/close/volume stay out of canonical SQLite.
- Two fills on 2026-10-08 at the supplied opening price 200; fees, virtual cash
  79,879.39698492462 and both positions reconcile between execution/backend.
- One aligned checkpoint per strategy/risk/planner; restart leaves fills, cash,
  positions and next fill ID unchanged, with no pending backend outbox.
- Authenticated dashboard Infrastructure returns actual host CPU/RAM/disk and 13
  project container observations; 40-second-old telemetry becomes unavailable.
  Container liveness does not set trading `ready`; clock sync remains unmeasured.
- Local daily capture uses the existing two-day/50-MiB writer. Test containers were
  stopped, with durable volumes and ignored evidence retained.

The actual monitor CLI also passed against the stopped fixture: all 13 services
remain STOPPED/CRITICAL, rather than showing healthy cached readings; all 13 daily
captures are bounded. The merged VPS logging overlay validated all 15 service/init
settings with syslog forwarding and two 25-MB fallback caches
(`storage/paper_final_packaging.log`). This verifies configuration and local capture,
not actual VPS syslog delivery or overnight rollover.

The initial fixture run correctly rejected its two-symbol/top-50 mismatch. The
accepted run aligns only fixture settings to two symbols; production paper stays
top 50. A separate two-candle public Binance probe succeeded from this local
network (`storage/paper_public_probe.json`); this is connectivity evidence, not an
actual-market overnight paper-trading acceptance.

All dashboard Go tests passed with the production CGO-disabled configuration;
market-data time, historical no-lookahead and default LIVE topology audits passed.
Paper runtime/API/frontend container builds passed. Step59 passed again with the
fingerprint below (`storage/paper_step59.log`), 25/25 matches, zero differences and
50 fills. No full-history or slow browser campaign was run.

Paper execution retains service close(T) monetary sizing and simulated open(T+1)
fills, including when starting later in the day. This proves operational wiring,
not achievable live prices, canonical next-open sizing parity or perpetual
collateral/funding economics. The VPS daily overlay covers all 13 long-running
services plus two initializers; the disk planning reserve is now 1.5 GiB for daily
archives and 24.5 GiB total. Actual VPS installation, rollover/trim, recreation,
overnight growth and capacity acceptance remain pending.

The full WSL Meson build passed with 20 configured targets after retiring the
frozen research runtime. Current executable names and binary paths were preserved;
the temporary comparison executable and its two support libraries were removed.
The compact Step59 gate passed after this cleanup:

```text
window             2020-01-01..2020-04-16
days               107
RealTest matches   25/25
differences        0
canonical fills    50
fingerprint        94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2
```

It covers deterministic reruns, system restart at a day boundary, dashboard
equality/restart, and pacing-speed invariance. Previously recorded live structural/
clock/SQLite/identity/deployment audits and historical no-lookahead tests also passed.

## Documentation/validation cleanup verified

On 2026-10-07 the WSL project build and Step59 gate were rerun successfully with the
same compact fingerprint above. Current LIVE structural/clock/daily/SQLite/identity/
deployment checks, the no-legacy-clock audit, historical topology/no-shared-clock
checks and the historical visibility no-lookahead unit test passed.

The dashboard source checker passed 33 contract markers. All 34 API routes are covered
by the updated guide; retained Markdown links resolve. Syntax checks passed for 86
validation/dashboard/handoff shell scripts and the modified Python checkers. The moved
dashboard source guide retains the ledger gate's required contract markers.

Removed 39 obsolete files (10 documentation files), moved five dashboard guides into
`dashboard/docs/` and renamed the current absence audit to
`validation/no_legacy_clock_audit.sh`. Current explanation text was consolidated
into the retained guides and `docs/README.md` index. The historical topology audit
now checks current read-only/query-only consumer ownership while allowing WAL sidecar
access on the existing writable named volume.

Frozen venue document bytes and generated evidence were preserved. No trading algorithm,
configuration or deployed topology was changed by this documentation cleanup.
Browser/full-history/deployed-runtime campaigns were not rerun.

## Research reporting migration verified

On 2026-10-07 the six frozen-runtime consumer sources were inventoried in
`research/REPLAY.md`. `algotrading_research_html` now uses CURRENT library APIs
for PureRSI and research-only Donchian/XH/XH-ATR experiments. The explicit
`algotrading_research_html_legacy` target initially preserved the original experiments;
all useful experiment consumers now have CURRENT paths. XH robustness, initial
parameter studies, statistics, BTC moving-average and isolated scenarios use CURRENT APIs.

The whole-project WSL Meson build passed with 23 configured targets (the additional
target is the explicit legacy HTML fallback). The bounded reporting gate passed
on `2020-01-01..2020-04-16`:

- 107 account snapshots and 25 closed PureRSI campaigns; baseline trade CSV
  byte-identical to the independently configured public fast executable.
- Eight compact sensitivity combinations: six successes, two invalid entry/exit
  rejections, zero failures. Repeated metric rows and HTML were deterministic.
- Existing sensitivity/grid/metadata CSV columns and HTML equity/drawdown,
  historical trade-return, seeded Monte Carlo and sensitivity sections preserved.
- Realized balance and marked equity reconcile to exported campaign PnL.
  Shorter-window account history matches the longer run's prefix; ten-bar
  warm-up yields no trades. Nonzero commissions are netted once.
- Synthetic fixtures verify open-cutoff exposure, closed-only Monte Carlo,
  observed-asset-bar holding duration (including data gaps), and Donchian close-signal/next-open execution
  with gap prices and entry-filter-independent exits.
- The bounded legacy fallback still generates all four original strategy reports.

Observed baseline economic differences are explicit, not regressions masked by
report compatibility:

| Study | Frozen final equity | CURRENT final equity |
| --- | --- | --- |
| PureRSI | 106521.266499368 | 106615.38896244 |
| Donchian | 97702.5021482057 | 97510.0451955678 |
| XH | 101370.95958945 | 100638.894859702 |
| XH-ATR | 91402.5261060157 | 91347.7321060525 |

Donchian retains its close-based signals but exits at next open rather than the
old same-close price. It has not entered the library's validated strategy catalog.
Reporting now derives missing holding counters from observed bars and includes
open campaigns in exposure; closed-trade metrics retain their realized-only basis.

Reviewable generated studies are local and ignored:
`storage/backtests/sensitivity_results/current_html_migration_20261007_rsi/` and
`storage/backtests/sensitivity_results/current_html_migration_20261007_donchian/`.
They include baseline metric CSVs, HTML, execution settings and CURRENT trades/
account snapshots. Validation logs are `storage/research_html_gate.log` and
`storage/research_html_step59.log`.

Step59 was rerun successfully with the accepted compact fingerprint above,
25/25 RealTest matches, zero differences and 50 fills, including deterministic
reruns, system/dashboard restart and paced-speed invariance. No full-history,
browser, private exchange or deployment campaign was run by this migration.

## XH stop-entry migration

The XH migration adds CURRENT one-bar stop entries and causal deferred exits.
Synthetic lifecycle validation passed: trigger/gap prices and monetary sizing,
expiry with missing assets, cancellation, split-fill idempotency, in-memory
restoration, adapter/wire/store rejection before unsafe routing, actual-fill fees,
open cutoffs and ATR ratchet reconstruction excluding entry-bar high.
`storage/xh_migration_stop_test.log` contains this proof. Step59 passed again with
the exact fingerprint above, including all restart/dashboard/pacing checks;
its log is `storage/xh_migration_step59.log`.

Reviewable CURRENT reports for all four definitions are ignored under
`storage/backtests/sensitivity_results/current_xh_migration_20261007/`.
Their XH/XH-ATR final equities are 100638.894859702 and 91347.7321060525,
with 20 and 12 closed campaigns respectively. The expanded 107-day reporting
gate passed, including four compact XH combinations and eight compact XH-ATR
combinations, all successful; legacy/current CSV columns, campaign uniqueness,
account/PnL reconciliation, shorter-window prefix equality and nonzero fees passed.
RSI fast-export equality and deterministic reporting also remain passing.
The whole-project WSL Meson build passed with all 23 configured targets.
Logs: `storage/xh_migration_gate.log` and `storage/xh_migration_full_build.log`.

Stop entries remain an in-process research capability. Service JSON, other
exchange adapters and durable schema v1 reject them. Disk restart and private
conditional routing are not claimed. The frozen XH sources normally look up a
future open while processing a close and fall back to that close at cutoff;
CURRENT waits for the actual later open and leaves cutoff positions marked.
ATR entry eligibility is evaluated at signal close rather than future fill-bar
ATR. No experiment was added to the validated strategy catalog.

## XH robustness consumer migration verified

`algotrading_research_xhbreakout` now links CURRENT `libalgolib` and shared
research XH definitions; build linkage has no frozen-runtime dependency. The
WSL target build and expanded reporting gate passed. Original full grids retain
2,508 XH combinations and 2,299 XH-ATR combinations, verified by enumeration
without executing those full studies. The 107-day compact study completed
eight successful combinations per strategy with zero invalid/failed runs.

Baseline and nonzero-fee trade/account CSVs are byte-identical to the CURRENT HTML
runner. Repeated exports/HTML are deterministic; shorter windows preserve prior
account history. Original sensitivity CSV columns, metadata columns (including
legacy fee factors), enabled axes and spacings remain. A synthetic zero-ATR
combination and an injected factory failure verify invalid/failed CSV rows,
reasons, empty metric cells and counters separately from successful runs.

Reviewable reports are ignored under
`storage/backtests/sensitivity_results/current_xh_robustness_20261007/`.
Build, reporting and release logs are `storage/xh_robustness_build.log`,
`storage/xh_robustness_gate.log` and `storage/xh_robustness_step59.log`.
Step59 passed again with the accepted fingerprint above: 25/25 RealTest matches,
zero differences, 50 fills and all rerun/restart/dashboard/pacing checks passing.
No new library/live behavior or conditional-routing support was added by this
consumer migration. The frozen runtime was retained for the remaining consumers;
the BTC study migration is recorded below.

## Initial parameter and statistics migration verified

On 2026-10-07 `algotrading_research_initial_params` and
`algotrading_research_stats` moved to CURRENT lib APIs and shared seven original
configurations. Their build linkage contains no frozen-runtime dependency.
The initial study retains original ranges, valid-combination filtering, marginal
medians and HTML filenames. Statistics retain logged metrics and an in-memory cache.
Both default to a cold 107-day daily study; full grids require explicit selection.

The WSL whole-project build passed with 23 configured targets. The expanded
`validation/research_html_report_gate.py` passed:

- Seven baselines and 82 valid compact runs; original 37,702 valid full-grid points
  verified by enumeration without running those full studies.
- Deterministic HTML/trade/account/sensitivity exports and exact shorter-window
  account prefixes for all seven strategies.
- Zero/nonzero-fee account reconciliation and long/short net PnL checks; logged
  statistics match the initial study at their published precision and trade counts.
- Initial PureRSI trade/account exports equal CURRENT HTML; existing HTML/fast,
  Donchian, XH and XH robustness checks remain passing.
- Focused fixtures cover short gap notional, bullish/bearish entry-bar protective
  ordering, later protective gaps, actual-fill cover sizing, split-fill/terminal
  idempotency, timed-exit bracket cancellation, missing short-entry expiry, observed
  holding periods and in-memory account/order/campaign restoration.

Reviewable reports and verification exports are ignored under
`storage/backtests/strategy_reports/current_initial_migration_20261007_verified/`.
Zero-fee baseline results for `2020-01-01..2020-04-16`:

| Strategy | Closed campaigns | Final marked equity |
| --- | ---: | ---: |
| BargainChaser | 6 | 102061.20580248954 |
| ATRBreakout | 66 | 101614.36781725891 |
| MRShort | 28 | 97947.91446917891 |
| PureMom | 3 | 95169.61563099793 |
| PureRSI | 25 | 106615.38896244048 |
| MRRSILong | 37 | 102796.47384186377 |
| XHBreakout | 2 | 97709.38798443158 |

CURRENT money comes from fills and executable-price sizing; no legacy future-open
lookup, signal-close quantity rounding or forced cutoff close-out remains in these
consumers. Source entry predicates and timed/stop rules remain explicit, including
MRShort's signal-close-plus-ATR protective stop and its research OHLC assumption.
These are research experiments, not additions to the validated strategy catalog.
Service JSON, unsupported adapters and durable schema v1 reject conditional fields;
stateful research strategies reject durable-store attachment. The in-memory proof
retains analytics alongside operational state; it does not prove disk/live recovery.
Sparse held-asset marking/target resolution still requires valid prices.

Logs: `storage/initial_migration_final_build.log`,
`storage/initial_migration_gate.log`, `storage/initial_migration_verified_study.log`
and `storage/initial_migration_release_gate.log`. Final Step59 passed with the
accepted fingerprint above, 25/25 RealTest matches, zero differences, 50 fills and
all deterministic/restart/dashboard/pacing checks passing. Full-history, slow browser,
deployment and private-exchange campaigns were not rerun.

## BTC moving-average migration verified

On 2026-10-07 `algotrading_research_btc_ma` moved to CURRENT lib APIs, reusing
the shared short/momentum configurations and portfolio sizing. Its original
baselines match the initial study; only BTC SMA length is swept, from 10 to 200
in steps of 5. There is no separate filter toggle in the source. CSV columns,
parameter medians, HTML filenames and metadata/run counters were retained.
The original sensitivity, parameter-grid and metadata writers were checked
unchanged against the tracked source. Bounded dates, fee/strategy controls,
overwrite protection and current baseline trade/account/metric exports were added.

The WSL whole-project build passed with 23 configured targets. The expanded
reporting gate verified all 78 combinations over `2020-01-01..2020-04-16`,
compact endpoints, deterministic CSV/HTML, baseline/nonzero-fee equality with the
initial study and exact shorter-history account prefixes. Injected invalid and
failed runs retain reasons, empty metric cells and correct counters. Original
full ranges remain in parameter metadata even in compact mode.

The saved bounded study completed 39 successful runs per strategy with no invalid
or failed runs. SMA-50 baselines: MRShort has 28 closed campaigns and final equity
97947.914469178912; PureMom has 3 and 95169.615630997927. Reviewable reports live
under ignored `storage/backtests/sensitivity_results/current_btc_ma_migration_20261007/`.
Logs are `storage/btc_ma_migration_build.log`, `storage/btc_ma_migration_gate.log`,
`storage/btc_ma_migration_study.log` and `storage/btc_ma_migration_step59.log`.
Step59 passed with the accepted fingerprint above and all replay/restart/dashboard/
pacing checks. Full-history, browser, deployment and private-exchange work were not run.

No new library/live behavior was needed. Conditional strategies retain the existing
research-only simulator and recovery limitations. At this stage frozen-runtime linkage
remained only for the explicit HTML comparison and `multi_strategy_main.cpp`; the
runtime was retained until useful consumer migration was complete.

## Isolated scenario migration verified

On 2026-10-07 `algotrading_research_multi_strategy` moved to CURRENT lib APIs for
all eight original strategy families. Each strategy/dataset pair still owns a fresh
strategy, indicator cache, account and campaigns. All 18 dataset identifiers,
filenames, benchmarks and regime-SMA bar counts were checked against the tracked
source, together with all original signal constants and 10% asset sizing.
The scenario-specific Bargain/ATR/short settings remain distinct from the initial study.
Shared short/momentum constructors accept an exact benchmark symbol, retaining BTC
defaults. Chart embedding and correlation presentation moved to the cohesive
`research/src/common/scenario_reports.*`; both original HTML writers are unchanged.

The whole-project WSL build passed with 23 configured targets. The bounded saved
study completed 24 runs with zero failures: eight strategies over 107 daily crypto
bars, 73 stock bars and 107 synthetic 4-hour keys in `20200101..20200416`.
All 18 scenarios remain available; the other real datasets were not replayed.
The focused `validation/research_scenario_gate.py` passed account reconciliation,
long/short net PnL, fees, duplicate-campaign checks, deterministic exports/HTML,
exact shorter-history prefixes and daily baseline/fee equality with the current
HTML runner. A bounded synthetic fixture warms the original 300-bar intraday filter
and exercises both short/momentum strategies with BTCUSDT rather than BTC.
Embedded benchmark curves match the selected input; the report's actual JavaScript
9-by-9 daily-return Pearson matrix, including BTC buy-and-hold, matches independent
calculations through Node without a browser. Missing/empty-input fixtures retain
skipped/failed counters and reasons and return nonzero status.
The existing HTML reporting gate also passed after the benchmark extension:
initial/stats, BTC SMA, PureRSI/fast, Donchian, XH/XH-ATR, robustness and the
focused stop/holding/accounting/recovery fixtures retain their accepted results.

Daily zero-fee results with this consumer's original settings:

| Strategy | Closed campaigns | Final marked equity |
| --- | ---: | ---: |
| BargainChaser | 6 | 101220.53107214371 |
| ATRBreakout | 39 | 102022.55998462629 |
| MRShort | 29 | 120253.6125956341 |
| PureMom | 3 | 95169.61563099793 |
| PureRSI | 25 | 106615.38896244048 |
| MRRSILong | 37 | 102796.47384186377 |
| XHBreakout | 20 | 100638.89485970164 |
| DonchianBreakout | 10 | 97510.04519556779 |

Reviewable reports and current campaign/account exports are ignored under
`storage/backtests/multi_strategy/current_scenario_migration_20261007/`.
Evidence logs: `storage/scenario_migration_build.log`, `scenario_migration_gate.log`,
`scenario_migration_study.log`, `scenario_migration_contracts.log`,
`scenario_migration_errors.log`, `scenario_migration_existing_reports.log` and
`scenario_migration_step59.log` under `storage/`.
Step59 passed with the accepted fingerprint above and all replay/restart/dashboard/
pacing checks. No full-history, browser, deployment or private-exchange work was run.

Synthetic intraday keys retain their original ordering; they are not reinterpreted
as real exchange timestamps. Annualization is explicit per bar frequency (252 for
stocks, 365 for daily crypto, intraday/multiday scaling). This remains isolated
research with existing conditional-order/recovery limitations, not validation of
a combined portfolio or new live strategies. At this point the frozen runtime
remained only for the explicit HTML comparison; its retirement is recorded below.

## Frozen research runtime retirement verified

On 2026-10-07 the temporary HTML comparison target, its conditional implementation
and all 62 frozen-runtime files were removed after all six useful consumers migrated.
Meson introspection confirms every consumer links CURRENT `libalgolib`; the whole
WSL build passed with 20 targets, removing the comparison executable and its two
support libraries. Current executable names and paths remain unchanged.

A bounded four-strategy study before and after removal produced byte-identical
HTML, trade exports and account history. Baseline metrics, parameter grids,
metadata and execution settings also match, excluding study identity/creation time.
Reports remain under ignored `storage/backtests/sensitivity_results/` in
`runtime_retirement_before_20261007/` and `runtime_retirement_after_20261007/`.
An ignored local archive preserves the removed files, including the modified README:
`storage/frozen_runtime_before_retirement_20261007.zip`.

The revised reporting gate passed without building or running a frozen engine.
It checks explicit CSV columns and HTML sections, all four CURRENT selections,
PureRSI/fast parity, accounting/fees/cutoff, deterministic reruns, causal prefixes,
initial/statistics parity, bounded BTC sensitivity and XH robustness/failure contracts.
Existing focused metrics and conditional-order fixtures also passed. No library/live
trading behavior changed. Research-only conditional-order/recovery limits remain.

Step59 passed with the accepted compact fingerprint above and all parity,
rerun/restart/dashboard/pacing checks. Evidence is in `storage/`:
`runtime_retirement_build.log`, `runtime_retirement_reports.log`,
`runtime_retirement_contracts.log`, `runtime_retirement_before.log`,
`runtime_retirement_after.log` and `runtime_retirement_step59.log`.
No full-history, slow browser, deployment or private-exchange work was run.

## Generated repository navigation verified

On 2026-10-07 `tools/generate_ai_index.py` generated three ignored local files:
`.ai/README.md`, `.ai/components.md` and `.ai/index.json`. The current snapshot
covers 628 files across 56 component directories. Its 20 literal Meson target
names match the configured build's introspection output. The Windows and WSL
scans produce identical index data; generated Markdown and include/reverse/candidate
links resolve to existing indexed source. Source bodies and absolute workspace
paths are not copied into the output.

Four focused WSL fixtures passed: force-tracked credentials/ignored paths and
symlinks stay excluded, stale edits/additions/deletions are detected, refreshes
are deterministic, CRLF/LF produce identical hashes, ambiguous includes remain
candidates and unrelated local `.ai/` notes are preserved. The generation guide
is `docs/codex/AI_INDEX.md`; original guides still own architecture, priorities
and accepted validation. No trading source or build definition changed.

The actual CLI detected guide edits with exit status 1 without changing outputs.
After refreshing the final guides, the freshness check passed. Step59 was rerun
under WSL and passed with the accepted compact fingerprint and all parity,
rerun/restart/dashboard/pacing checks. No full-history or browser campaign ran;
the whole Meson build was not repeated because compiled sources/build definitions
were unchanged.

Evidence logs under `storage/`: `ai_index_tests.log`, `ai_index_generation.log`,
`ai_index_stale.log`, `ai_index_freshness.log` and `ai_index_step59.log`.

## Daily service logging implemented and locally verified

The initial five-day policy below was superseded on 2026-10-08 by the user's
two-day, size-bounded policy; see the current validation entry below.

On 2026-10-07 optional LIVE/dashboard Compose overlays and a dedicated loopback
rsyslog receiver were added. The host archive uses UTC receipt-day files under
`/var/log/algotrading/services/`, with private permissions, defensive credential
filtering and append across receiver restarts. An hourly persistent systemd timer
retains today and the previous four UTC days, including quiet services. Cleanup
preserves symlinks, unknown files and trading journals/checkpoints. The overlays
force C++ stdout logging instead of optional unbounded application file output.
Base Compose settings and trading source remain unchanged.

Four focused WSL tests passed using an isolated real rsyslog TCP receiver and
temporary fixtures: retention/safe cleanup scope, restart/append and credential
filtering, UTC date templates, and all eight LIVE/two dashboard merged logging
settings. Permissions were checked. Compose rendering used dummy values; no LIVE
containers were deployed. The date-boundary fixture substitutes reported time
into the production date template; actual UTC receipt-time rollover on the VPS
remains an acceptance check. The LIVE topology audit and systemd unit verification
passed. Step59 passed with the accepted compact fingerprint, 25/25 matched trades,
50 fills, zero differences and all rerun/restart/dashboard/pacing checks.

Evidence under `storage/`: `daily_logs_tests.log`, `daily_logs_topology.log`,
`daily_logs_units.log` and `daily_logs_step59.log`. No full-history or slow browser
campaign ran. Installation instructions and delivery limits are in
`deploy/live/README.md`: the non-blocking forwarding buffer can lose diagnostics
during prolonged receiver outages, and archive filtering does not sanitize
upstream Docker caches. This archive is separate from durable trading evidence.
Actual VPS installation, Docker-to-host delivery, container recreation, real UTC
rollover and scheduled expiry remain pending; nothing was deployed.

## Local VPS equivalence baseline verified

On 2026-10-07 the current WSL whole-project Meson build passed and the LIVE
runtime bundle was regenerated with six executables and 39 shared libraries.
A separate `algotrading-runtime:vps-baseline` image built successfully; all six
packaged binaries exited successfully with `--help` in read-only containers with
no network, capabilities or trading-data mounts. Existing deployment image tags
were not replaced, and no trading services were started.

The LIVE topology audit and four daily-logging fixtures passed. Step59 passed
again with the accepted compact fingerprint, 25/25 matched trades, zero
differences, 50 fills and all rerun/restart/dashboard/pacing checks. Replay input
and default LIVE market/strategy/portfolio configuration checksums, actual Meson
compiler/build options, local tool versions and the validation image ID were
captured under ignored `storage/`. Source navigation is refreshed separately;
the index excludes credentials/generated state and is not a deployment manifest.

Evidence: `vps_baseline_build.log`, `vps_baseline_bundle.log`,
`vps_baseline_topology.log`, `vps_baseline_logging.log`,
`vps_baseline_step59.log`, `vps_baseline_image_build.log` and
`vps_baseline_image_smoke.log`. Configuration/input/toolchain captures use the
same `vps_baseline_` prefix. The build reported existing unused-variable warnings
in the fast research entrypoint; no compiled source changed in this task.

`deploy/live/README.md` now defines the local-to-VPS comparison procedure.
Actual VPS verification remains pending: no SSH host/user/repository location was
provided, no remote connection was made and no deployment was performed. The
local image smoke check does not prove deployed messaging/database recovery or
private exchange connectivity. No full-history or browser test ran.

## Isolated transport/persistence crash recovery verified

On 2026-10-07 `validation/transport_persistence_integration_test.py` passed under
WSL against real, disposable NATS 2.10/PostgreSQL 16 containers, compiling the
current `JetStreamBus` and `PostgresStateStore` source. A forced SQL constraint
failure after fill insertion rolled back both the fill and snapshot. The fixture
then committed a fill/snapshot and abruptly exited before ACK. After broker and
database kill/start, a fresh consumer recovered exact cash, position, decision/
execution timestamps, next-order identity and processed fill IDs, rebound to its
wildcard durable subscription and ACKed redelivery without another economic delta.
Duplicate publication/audit insertion stayed singular; an incompatible durable
filter was rejected. A second infrastructure kill/start preserved the consumed ACK
state and account/fill evidence. All UUID-owned test containers and their anonymous
volumes were removed; the final cleanup capture is empty.

The fixture handler owns economic deduplication using recovered processed-fill
IDs. The store's unique fill audit row alone does not make repeated account deltas
safe. This checks actual adapter transactions/delivery and infrastructure restart;
it does not run the five-service LIVE chain, service checkpoint/outbox crash paths,
container recreation, private routing or a VPS. Those acceptance checks remain
pending. No trading source, base deployment or Meson build definition changed.

Step59 passed again with the accepted compact fingerprint, 25/25 matched trades,
zero differences, 50 fills and all rerun/restart/dashboard/pacing checks. Evidence
under ignored `storage/`: `transport_persistence_integration.log`,
`transport_persistence_cleanup.log` and `transport_persistence_step59.log`.
The fixture compiled its affected adapters directly; the whole Meson build was
not repeated after the prior accepted build. No full-history/browser test or
LIVE/VPS deployment ran. The validation/deployment guides and roadmap now identify
the fixture and its acceptance boundary.

## Controlled local service environment prepared

On 2026-10-07 `validation/local_service_campaign.py` prepared an isolated Docker
project from the current historical topology, with its own internal network,
NATS/PostgreSQL/SQLite volumes, pinned runtime image and generated test credentials.
The seven current service binaries were built under WSL and packaged successfully.
Existing LIVE/historical state, image tags and trading source were preserved.

The bounded synthetic fixture contains 111 BTC/ETH daily rows per coin. Only the
April 19 close creates the economic bootstrap cycle; April 20 supplies execution
opens. The smoke check and raw duplicate injections passed: one checkpoint per
strategy/risk/planner at `20200419`, two fills at `20200420`, no extra fill IDs,
drained outbox and matching execution/backend cash and positions. This proves a
bounded simulated service chain, not private exchange or strategy performance.

The outbox publication-receipt crash fixture passed after recovery. It observed the
SQL barrier after publication, killed the backend, verified pending receipts and
recovered all six records without economic duplication. Execution-state container
recreation also preserved the accepted account/fill/checkpoint evidence. CPU/RAM
sampling and summary generation ran; these brief samples do not establish capacity.
On fresh project `algotrading-local-6f2aa7909ee4`, the strategy publish-before-checkpoint
barrier passed: the checkpoint remained absent after the observed crash, and durable
redelivery produced the accepted aligned pipeline and two fills. Both accepted
projects were paused with their volumes and evidence retained.

The harness includes guarded service/infrastructure kill/start/recreation commands,
fresh-project publish-before-checkpoint SQL barriers, an outbox publication-receipt
crash fixture, daily append-only log capture with five-day cleanup and attributed
CPU/RAM samples plus a summary. Its runbook is
[validation/LOCAL_SERVICE_CAMPAIGN.md](validation/LOCAL_SERVICE_CAMPAIGN.md).
Risk/planner checkpoint barriers, infrastructure loss/recreation, the complete fault matrix,
overnight rollover/scheduled expiry and full-backtest/live resource profiles remain
pending campaign acceptance. A manually restarted service does not prove unattended
recovery. Local log capture does not install the VPS rsyslog/systemd receiver.

Preparation initially rejected a CRLF synthetic CSV header; the fixture now writes
LF explicitly and a fresh project passed. Host TCP publication was blocked by the
internal network; duplicate injection now runs an SDK client inside that network.
Restart checks exposed unordered order-map/fill-set serialization: the checker sorts
these two collections for comparison while preserving every record and duplicate.
The original saved baseline remains intact; cash, positions, fills and checkpoint
payloads must still match exactly.
Checks already running with the old comparison timed out after recreation; a fresh
run with the corrected comparison passed, including a repeated outbox crash.

The whole WSL build passed. A redundant second compile encountered a recovering
Ninja dependency cache on the Windows-mounted build directory and was stopped;
the missing ignored library artifact was restored from the just-validated bundle.
Fresh sandbox packaging reused that accepted build with `--skip-build`. Use a
configured Linux-filesystem `BUILD_DIR` for sustained campaigns. This workaround
does not accept a failed source build or permit reuse after compiled-source changes.

Step59 passed with the accepted compact fingerprint, 25/25 matches, zero differences
and 50 fills, including restart/dashboard/pacing checks. Four daily logging tests
and four generated-index tests passed. Evidence stays ignored under
`storage/local_service_campaign/`; the accepted sandbox is
`algotrading-local-39ab90a639d5`. Build/fixture failures were retained separately,
and only the failed generated project's containers/network/volumes were removed.
Read-only guard checks rejected repository/LIVE paths and a remote Docker endpoint.
No full-history/browser test, VPS deployment or private trade ran.

## One-command local CPU/RAM study implemented and validated

On 2026-10-07 `research/replay.py resources --days 100 --seconds 300 --open`
was added to the current public CLI. It reuses canonical dashboard replay with
original inputs and direct RealTest comparison in a separate container while the
complete synthetic service sandbox plus API/web/watchdog/test-file notifier runs.
The dashboard observes canonical simulation state; it does not reinterpret the
synthetic service fixture as a 100-day economic backtest. Every run owns its
project, loopback web port, volumes, images, copied runner and output/state paths.

The five-minute WSL campaign passed on project `algotrading-local-0dcbdd742311`:

- Measured `300.000277` seconds, with 295 complete snapshots of all 14 containers.
- Original-data replay covered `2020-01-01..2020-04-09` (100 days), completed around
  160.64 seconds, produced 40 canonical fills, and ended CLEAN/route-safe.
- RealTest: 22/22 matches, zero differences; 18 closed campaigns and four open
  campaigns compared under the existing partial-cutoff entry-only contract.
- Authenticated dashboard client: 349 successful reads and five failures during
  initial simulation-state warmup. All four dashboard services were healthy while
  the remaining period was measured. The generated project stopped automatically;
  a subsequent Docker query confirmed no running project containers.
- Container working-set RAM: mean 112.17 MiB, sampled peak 126.77 MiB. CPU sum:
  mean 12.94%, sampled peak 46.14%, where 100% represents one logical core.
- Separate Windows samples were available: whole-host CPU mean 25.4%, sampled
  peak 100%; used host RAM mean 9957.99 MiB and peak 10393.46 MiB. These include
  unrelated applications and observer overhead. Browser/VM process working sets
  overlap host memory and must not be added to container totals.

Setup/build/startup and shutdown/comparison happen outside measurement. Deliberate
display delays add roughly 25% of the selected duration to processing; this is a
bounded observational load, not maximum-speed throughput or a VPS-sizing acceptance.
Existing dashboard CPU/RAM limits remain. The watchdog retains its required public
TESTNET/dry-run settings with venue egress blocked by the internal network; no private
order or Telegram delivery is enabled. Remaining failure/recreation/overnight
retention tests are deferred to the VPS at the user's request.

`research/resource_profile.py` owns lifecycle/sampling/read-only load;
`resource_report.py` writes offline selectable/hoverable graphs and sample summaries.
Missing/stale observations remain gaps, and failures retain partial HTML evidence.
Earlier short attempts rejected incomplete replay/coverage; Docker Desktop terminal
redraw sequences are now removed before parsing. A corrected 20-day/30-second
campaign passed, including Windows sampling and automatic shutdown. Four focused
report tests and a generated-JavaScript syntax check passed. Credential environment
and key files are excluded from both dashboard Docker build contexts, including
nested files. Generated reports, telemetry, credentials and replay exports are ignored.
After copying the runner into each private run and adding end-of-window service
checks, a final 35-day/60-second campaign (`algotrading-local-777efdd1d8f3`) passed
with actual fills and 5/5 RealTest matches, zero differences. It verified the final
code path, report formatting and automatic shutdown without repeating five minutes.
Four generated-index fixture tests also passed.

Step59 passed again with the required compact fingerprint, 25/25 matches, zero
differences and 50 fills, including all restart/dashboard/pacing checks. Its log
is ignored `storage/resource_profile_step59.log`. No full-history run, slow browser
campaign, VPS deployment, private trade, commit or push occurred.

## Earlier evidence, not rerun by the readability cleanup

Full history: `2020-01-01..2025-10-13`, 2113 source days, 631 candidate
trades, 628 fully matched, four comparison differences, 1261 fills.

```text
1551106eac4dd7b712729f72980ac196308d61b1d7cbb9e538c557271415cbf0
```

The differences are the BNB exit date, missing candidate FET trade, final ZEC
end-state/time, and extra candidate FET trade. Their review was closed by the user
on 2026-10-08 based on closing conventions and RealTest RSI rounding. They remain
visible; this acceptance does not establish exact parity or add a mismatch whitelist.
Earlier full-history dashboard/system
fills were byte-identical.

A warmed later-date pacing proof recorded:

```text
b85026fe0cf5206dc4120af9b3a12cdc6a2a978d08faf69b0dc99b602bbe9024
```

These recorded proofs do not imply that every legacy validation script is usable.

## Current code and readiness

- Current library filenames are descriptive; supported source no longer uses
  version suffixes. Wire/schema/version labels remain unchanged.
- PureRSI is the only supported strategy under `lib/src/strategy/validated/`.
  `on_hold/` experiments are excluded from the supported build.
- Live services retain cohesive state machines and meaningful local components.
  There is one source-level build file per service.
- `research/replay.py {fast|system|dashboard}` is the public replay interface.
- `.ai/` provides generated local source navigation; refresh/check it with
  `tools/generate_ai_index.py`. Its outputs remain ignored by Git.
- The frozen research runtime and comparison target have been removed.
  The HTML source builds CURRENT PureRSI and research-only Donchian,
  XH and XH-ATR reporting with explicit contract/accounting regression checks.
  XH robustness, initial parameter, statistics, BTC moving-average and scenario
  sources now link CURRENT lib. All six useful consumers have current paths.
- Dashboard private routing remains blocked. Public venue metadata does not
  establish private account/order connectivity.
- Slow browser acceptance, deployed NATS/PostgreSQL integration, VPS equivalence,
  private TESTNET/shadow and MAINNET readiness remain separate outstanding work.
- Local NATS/PostgreSQL adapter integration and bounded simulated service
  pipeline/duplicate checks pass; complete service fault acceptance remains pending.
- Persistent service/day logging and two-day bounded cleanup are implemented and locally
  tested; installation and acceptance on the VPS remain pending.

## Local disk capacity inventory

On 2026-10-08 WSL `du`, file metadata, `docker system df -v` and selected image
inspection completed without starting/stopping containers, pruning or changing data.
Evidence is ignored `storage/disk_capacity/`: `inventory.json`, `file_sizes.tsv`,
`component_sizes.tsv`, `docker_usage.txt` and `docker_summary.jsonl`. Measurements:

| Item | Observed size / scope |
| --- | --- |
| LIVE runtime bundle | 92,566,236 bytes (88.3 MiB) |
| Selected current-equivalence runtime, profile API/web, PostgreSQL and NATS images | About 1.26 GB summed `system df` rows; conservative because layers are shared |
| LIVE configured local market database including WAL/SHM | 6,438,544 bytes (6.14 MiB); not a proposed VPS state restore |
| Canonical original-data CSV + RealTest reference | 9,772,201 bytes (9.32 MiB) |
| Local research database/data collection | 6,919,188,970 bytes (6.44 GiB), mostly parquet/intraday files not used by default LIVE |
| Five-minute sandbox named state | Market 4.235 MB, PostgreSQL 48.96 MB, NATS 106.9 kB, watchdog 5.045 kB, notifier 10.92 kB |
| Five-minute sandbox captured redacted Compose log | 387,532 bytes; includes setup/shutdown, not a steady log growth measurement |
| All local Docker images / volumes / build cache | 3.96 GB / 693.4 MB / 23 GB; accumulated local experiments, not deployment requirements |

Docker Desktop `image inspect Size` reports different values from `system df` on
this image store; both are retained, and the larger `system df` image rows inform
the deployment allowance. The image IDs are recorded, but local images are not
proof of destination build/toolchain equivalence. Source checks confirmed age-only
five-day log cleanup, nominal roughly 920 MB configured Docker cache/log budgets
with optional gateway and four dashboard services, no explicit JetStream stream
age/byte cap, accumulating durable audit/fill history and download-only symbol
retention rather than OHLCV deletion. No durable retention policy was changed.
The dashboard daily overlay currently covers API/web only; watchdog/notifier still
have size-only production JSON logs. Cover these in the forthcoming VPS configuration
before accepting the all-service persistent daily logging requirement.

The initial 60 GB disk is a reasonable candidate: the updated LIVE runbook allocates
24.25 GiB of planning reserves, leaving roughly 31 GiB before filesystem overhead
on a decimal 60 GB disk. These are estimates, not enforced caps. Actual Linux/package
size, steady-state log/database growth, backup size and VPS capacity remain unmeasured.
Verify them overnight and target at least 15 GiB free. Transfer selected source and
validation inputs rather than the whole local storage/Docker cache; preserve live
state and use a consistent backup if restoring. No VPS was purchased or deployed.
This inventory changed documentation only; no build, Step59 or backtest rerun was
needed. The previously accepted compact fingerprint remains the baseline.

## Two-day bounded service logging locally validated

The initial 10 MiB cap was raised to 50 MiB later on 2026-10-08 at the user's
request. All eight WSL receiver/retention tests passed again, including a real
oversized-file fixture against the 50 MiB default; evidence is
`storage/fifty_mib_logs_tests.log`. The logging threshold and disk-budget figures
changed; the previously accepted Step59 result below was not rerun for this
constant-only adjustment.

On 2026-10-08 the user replaced the original five-day diagnostic policy with
today/yesterday and size trimming. The host receiver now passes redacted UTC
records through rsyslog `omprog` to the existing `clean_daily_logs.py --receive`.
That writer checks the 50 MiB limit before each append, keeps roughly the newest
25 MiB of complete lines when trimming, adds a `LOG-TRIM` marker and continues in
the same dated inode. It never creates numbered/compressed archives. An individual
oversized record is replaced with an omission marker. File locks serialize append
and timer/manual trimming; diagnostic expiry runs on startup, date changes and a
persistent UTC minute timer, including quiet services. Midnight expiry can lag one
timer interval. Today/yesterday are at most two dated files per service, not two
files for the entire stack. Durable trading journals/fills/checkpoints remain outside
this policy. Docker inspection caches retain their separate size limits.

Eight WSL tests passed with rsyslog 8.2112.0 and isolated temporary fixtures:
two-day UTC retention/safe scope, receiver restart/recreation and permissions,
credential filtering, midnight templates, real receiver burst trimming followed
by continued writes/restart, tail/complete-line preservation, oversized-record and
unsafe-path handling, actual default 50 MiB enforcement, and unchanged merged
Compose service settings. LIVE topology audits, Python syntax and systemd unit/
minute calendar checks passed. Systemd reported only an unrelated installed snapd
`RestartMode` warning after checking permissioned temporary unit copies.
Evidence: ignored `storage/two_day_logs_tests.log`, `two_day_logs_topology.log`,
`two_day_logs_units.log`, `two_day_logs_timer.log` and `two_day_logs_step59.log`.
The WSL Step59 release gate passed after this logging change: 25/25 matches,
zero differences, 50 fills and fingerprint
`94fdf8d84607dd31c8fa04ecde571738dfabfc234dedff75cd133793dc768da2`,
including deterministic rerun, system/dashboard restart and pacing invariance.
No full-history or slow browser test ran.

The maintained deployment/dashboard guides and roadmap now use two-day capped
logs. Twelve stable service identities need at most 1,200 MiB of retained archive
contents; the planning archive allowance is 1.25 GiB and total disk reserves are
24.25 GiB. The dashboard daily overlay still needs watchdog/notifier coverage in
the next VPS configuration task. No host units were installed, no VPS was deployed
and no existing evidence or trading state was deleted. Actual Docker-to-host
delivery, real midnight expiry and VPS restart/retention acceptance remain pending.

## Next task

The original six-consumer source inventory is in `research/REPLAY.md`. All useful
consumers now have current paths and the frozen runtime is removed. Generated
`.ai/` source navigation is available. The bounded local resource study is available
as a single command and its five-minute campaign passes. At the user's request,
remaining service fault/recreation/overnight-retention acceptance is deferred to
the VPS. Local disk inventory is complete. Prepare the Linux 2-vCPU/2-GB sandbox
deployment configuration and transfer scope next, then create the VPS when requested.
Install and accept persistent daily logging, verify VPS equivalence
and continue private TESTNET/shadow readiness. Keep those roadmap checks pending;
the local resource report does not accept VPS hardware or real trading capacity.

`docs/ROADMAP.md` now owns the full ordered pending requirements, including the
user's dashboard chart/timeframe and empty-section investigations, funding/BTC
collateral, Telegram setup, Manual Control and simplest-backtest comparison,
complete-backtest/live CPU/RAM profiles and gradual small-capital testing.
Kraken is the likely initial live venue, subject to a Kraken/Hyperliquid perpetual
coverage comparison over the same top 50 Binance coins by the last 25 completed
days' volume and verification of account/collateral eligibility. Unavailable
perpetual positions must be skipped without an unsupported fallback. The unfinished
exchange-analysis criterion remains open. These are recorded requirements;
this documentation update did not implement features, run the venue study or
authorize private trading/deployment.

See the [handoff](docs/codex/CONTEXT_FULL.txt), [architecture](docs/codex/ARCHITECTURE_FULL.md)
and [validation guide](validation/README.md) for navigation and checks.
