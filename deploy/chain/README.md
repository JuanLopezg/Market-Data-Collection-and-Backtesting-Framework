# Chain: runtime layout and launcher

One installation per machine. Every runtime file lives under one root folder:
`/algoTrading` for the preferred VPS layout, any Linux folder locally (for example `~/algotrading-runtime` in WSL;
not `/mnt/c`, because PostgreSQL needs real file ownership).

The VPS was relocated from `/` to `/algoTrading` on 2026-10-14 with state preserved.
The operating examples below use the deployed root.
`algo install` renders host systemd, logging and AppArmor templates for the selected
root. Docker mount destinations keep their existing container paths. Linux-owned
paths such as `/etc`, `/usr` and `/run`, and Tailscale state, remain OS-managed.

Installing at a new root does not migrate an existing installation. Any later
authorized relocation must preserve all eight application folders, replace the
installed host units/AppArmor rules and rebase absolute host paths in the active
comparison study/manifest before restarting. Historical evidence stays intact.

```text
<root>/
├── chain/      compose.yml, algo, chain.env, monitor.py, logging/   (what you operate)
├── config/     non-secret configuration: strategies/, portfolio/, market_data.json
├── secrets/    chain.env with credentials (0700/0600; never print or copy it around)
├── data/       durable state, one folder per owner (MODE=paper)
│   ├── postgres/    orders, accounts, checkpoints, positions and daily equity
│   ├── nats/        JetStream messages
│   ├── market/      database.db (candles, quote volume)
│   ├── dashboard/   watchdog/, audit/, alerts/, notifier/
│   ├── manual/      Manual Control requests/ and processed/ (dash-api <-> execution)
│   ├── live/        MODE=live: its own postgres/, nats/, market/, dashboard/, venue/, manual/
│   ├── telemetry/   host.json from monitor.py (regenerated, not backed up)
│   └── receiver/    log receiver state (not backed up)
├── logs/       <service>/YYYYMMDD.log, today and yesterday, at most 50 MiB per file
├── reports/    generated outputs to read
├── backups/    YYYYMMDD_HHMMSS.tar.gz of data/
└── releases/   image archives per version (rollback)
```

Only `data/`, `config/` and `secrets/` must survive a reinstall. `chain/` is replaced
by `algo install`; `logs/` and `reports/` can be deleted.

`reports/` (written by `dash-api` every 10 minutes from ExecutionState's position ledger):
`closed_positions.csv` and `closed_positions_<YEAR>.csv` (by UTC close year), one row per
position from flat to flat: asset, side, opened/closed, quantity, entry/exit price, cost
basis, proceeds, gross PnL, fees, funding (+ received) and net PnL = gross - fees +
funding, in USD. Use them for the yearly tax report and to match the exchange export.
Funding is recorded only from a real venue (`trading_funding`); the PAPER simulation has
none, so it is 0 there. In LIVE, Kraken reports fees and funding in its account history,
not on the fills: the gateway reads it every 5 minutes (and 30 s after a fill), and
execution books each entry once (`trading_venue_costs`), so a position's fees and funding
fill in while it is open and are final when it closes.

Accounting owned by `execution` (PostgreSQL, rebuilt from `trading_fills` plus
`trading_funding` at every start, so it can never drift from the fills):
`trading_open_positions` and `trading_closed_positions` (average-cost basis, realized PnL,
fees, funding per position) and `trading_equity_daily` (cash + positions marked at each
daily close; missing days since the first fill are backfilled from the fills). In
`MODE=live` each row also has the venue interest and net USD capital flows up to that day
and the BTC collateral (after haircut) that day was sized with; equity excludes the
collateral. The strategy sizes with equity + collateral (`trading_daily_collateral`).

## Services

| Service | Role |
| --- | --- |
| `nats`, `postgres` | messaging and durable state |
| `market-data` | Binance public candles into `data/market/database.db` |
| `strategy` | PureRSI intents from completed candles |
| `risk` | portfolio risk and target weights |
| `planner` | notional order plans |
| `execution` | order state, fills and accounting; reconciles with the exchange at startup, hourly 01:00-23:00 UTC and at 23:50 (a mismatch pauses trading until a clean check); executes Manual Control requests |
| `gateway` | MODE=paper: exchange commands to the simulated backend |
| `sim-exchange` | MODE=paper: virtual account and simulated fills |
| `gateway-kraken` | MODE=live: exchange commands to Kraken Futures (journal in `data/live/venue`) |
| `dash-api`, `dash-web` | dashboard API and web on `127.0.0.1:DASHBOARD_PORT` |
| `watchdog`, `notifier` | alert evaluation and delivery (test file or Telegram) |
| `data-init`, `market-data-init` | one-shot folder ownership and market schema setup |

The container name equals the service name, so logs land in `logs/<service>/`.
Each line is exactly what the service wrote:
`YYYY-MM-DD HH:MM:SS.mmm [LEVEL] Class::function : message` (UTC).

## Private mobile dashboard access (Tailscale)

The VPS uses host-installed Tailscale with `tailscaled` enabled at boot. Its
`algotrading` node and the phone belong to the same private tailnet. Enrollment
uses `--accept-dns=false --accept-routes=false`; HTTPS certificates are enabled
in the Tailscale console. Funnel is not enabled.

With Tailscale connected on the phone, open:
`https://algotrading.taild27a89.ts.net/` and use the existing dashboard login.
The PC and its SSH tunnel are not required. The dashboard remains bound to
`127.0.0.1:8092`; host Serve supplies the private HTTPS proxy.

```bash
sudo tailscale serve --bg http://127.0.0.1:8092
sudo tailscale serve status
sudo systemctl status tailscaled
# Disable only mobile dashboard serving:
sudo tailscale serve --https=443 off
```

Serve configuration persists in the host Tailscale state. Check node key expiry
in the Tailscale admin console for long-term access and review tailnet permissions
before adding other members/devices. HTTPS device names appear in public
certificate-transparency records; dashboard content remains private.
Actual phone login and chart acceptance are tracked in CURRENT_STATE.md.

## Commands

```bash
deploy/chain/algo install <root>   # from the repository: create or update an installation
deploy/chain/algo build            # from the repository: build the images named in chain.env

<root>/chain/algo up               # everything (starts log receiver and host monitor if needed)
<root>/chain/algo up strategy      # one component and what it depends on
<root>/chain/algo down risk        # stop one component
<root>/chain/algo down             # stop everything; data is kept
<root>/chain/algo restart planner
<root>/chain/algo ps
<root>/chain/algo logs market-data
<root>/chain/algo show strategy    # mounts, ports, command (secrets masked)
<root>/chain/algo backup
```

`install` never overwrites `chain.env`, `config/`, `secrets/` or `data/`. The dashboard
viewer and operator passwords are in `secrets/chain.env` (`DASHBOARD_OPERATOR_PASSWORD`;
an installation created before Manual Control has none: add the line to enable the
operator login).

## Manual Control (PAPER and LIVE)

The dashboard page Manual Control (OPERATOR login) takes a target portfolio CSV
(`asset,weight_pct`, % of equity; CASH optional, held assets missing from the file are
closed). Flow:

1. `dash-api` checks the file against the portfolio risk limits (the strictest strategy's
   `max_asset_weight` and `max_gross_leverage`) and the tradable assets (PAPER: the
   top-50 universe and held assets; LIVE: Kraken contracts verified on the Symbol Mapping
   page), shows the change at the latest daily closes, and on confirmation writes
   `data/manual/requests/<time>-<id>.request`.
2. `execution` picks it up within a second and checks again: reconciliation clean, no
   order open, the daily cycle finished, not between 23:45 and 00:15 UTC, request less
   than 2 minutes old, limits. It reads the current Binance prices
   (`/fapi/v1/ticker/price`, at most 5 minutes old), sizes every strategy's orders on its
   share of equity with the normal planner and sends them through the gateway. Anything
   that fails is rejected with its reason; nothing waits in a queue.
3. PAPER: the simulated exchange fills them at once at those prices (normal commission).
   LIVE: Kraken executes them at the market, with the gateway's usual checks (symbol map,
   USD value within 1.5x of the planned value) and only when `LIVE_ORDER_SUBMISSION=enabled`
   (otherwise the page shows `LIVE_ORDER_SUBMISSION_DISABLED`).
4. The outcome is written to `data/manual/processed/` and shown as a progress bar
   (checked, orders at the exchange, filled). The next 00:00 UTC daily cycle returns to the
   strategy targets: its account snapshot carries `resync_to_targets`, so risk moves every
   position back to its approved weight, including positions it would otherwise hold.

Every request is also in the Alerts & Audit history (`data/dashboard/audit`).

## Settings

`chain/chain.env` (non-secret): images, `STRATEGY_FILE`, `PORTFOLIO_FILE`, the RSI
values shown by the dashboard (keep them equal to the strategy file), initial cash,
commission, warmup days, dashboard port and notifier sink.

`secrets/chain.env`: PostgreSQL and dashboard passwords (viewer, operator), optional
Telegram bot settings.

`MODE` selects the venue: `paper` (default) or `live` (next section). `./algo` adds it to
`COMPOSE_PROFILES`; list only extra profiles (`kraken`) there.

## Switching to live (MODE=live, Kraken Futures)

Nothing here is enabled by default, and each step needs the user's authorization. LIVE
uses its own trading state in `data/live/` (NATS, PostgreSQL, market data, dashboard
stores, venue journal): the PAPER history stays in `data/` and is never mixed with real
money. Internal stream and database names keep their `PAPER` spelling in both modes.

1. Trading key: create a Kraken Futures key with trading permission and **no withdrawal
   permission**. Write `<root>/secrets/kraken.env` with `KRAKEN_API_KEY=` and
   `KRAKEN_API_SECRET=`, readable by the runtime user only:
   `sudo chown 10001:10001 /secrets/kraken.env && sudo chmod 400 /secrets/kraken.env`.
2. `chain.env`: `LIVE_INITIAL_CASH` = the account's USD balance at the switch (the startup
   reconciliation compares it with Kraken), `LIVE_ACCOUNT_NAMESPACE` = a short new name
   (part of every client order id; use a new one after any reset of `data/live`).
   Keep `LIVE_ORDER_SUBMISSION=disabled`.
3. Stop PAPER **before** changing the mode: `algo down`; then set `MODE=live` and
   `algo up`. `algo up` refuses to start while a container of the other mode runs (two
   gateways would send the same orders to two venues).
4. Read-only first: with submission disabled the gateway only sends GET requests. Check
   the startup reconciliation in `logs/execution`, the Symbol Mapping page and that no
   strategy asset is reported as not tradeable. LIVE reconciliation accepts 0.50 USD of
   cash difference (venue rounding; PAPER is exact).
5. Real orders: set `LIVE_ORDER_SUBMISSION=enabled` and `algo up gateway-kraken dash-api`
   only for an explicitly authorized test; set it back to `disabled` afterwards. A first
   small test can use Manual Control (one asset, a small weight). Optional:
   `LIVE_DEAD_MANS_SWITCH_SECONDS` (Kraken cancels open orders if the gateway goes silent).

Live vs Expected in LIVE: with `PAPER_COMPARISON_FILE=/data/comparison/comparison.json`
in `chain.env`, restart `algotrading-comparison` after the switch (it reads `MODE` from
`chain.env`). The study is kept in `data/live/comparison/` and starts from the first
observed cycle; from the next one each row compares the Kraken account with the fast
backtest (same frozen market data and decisions, Binance open of the execution day,
`LIVE_COMMISSION_RATE`, default 0.0005 = Kraken taker fee at the lowest tier, the same BTC
collateral for sizing and the same USD deposits/withdrawals) and splits the difference into
real versus expected fees, funding, interest, fill prices against the Binance open and the
markets Kraken skipped. Fees and funding arrive after the fills, so the latest row keeps
updating until the next cycle. Manual Control trades are not in the expected run.

Back to PAPER: `algo down`, `MODE=paper`, `algo up` (PAPER state was untouched).

## Log receiver and monitor

Docker sends every container's output to `127.0.0.1:5514`. The receiver
(`chain/logging/rsyslog.conf` + `clean_daily_logs.py`) stores one file per service and
UTC day and keeps today and yesterday. On the VPS it runs as `algotrading-logs.service`
with the minute cleanup timer (`LOG_MODE=receiver`). Where systemd does not run it,
`algo up` starts it as your user.

Docker Desktop (local WSL) cannot reach a receiver on the WSL loopback, so `install`
sets `LOG_MODE=local` there: `compose.local.yml` keeps Docker's json-file logs and
`monitor.py --log-root` copies them with the same writer into the same files.
`monitor.py` writes `data/telemetry/host.json` for the dashboard Infrastructure page and
appends `data/telemetry/history.jsonl` (one row every 30 s, seven days kept) for its CPU/RAM/disk
charts. It is started detached (`setsid`) and `algo` waits for its first sample.
Each container's square on that page is Docker's health check: `nats` asks its
`/healthz` (server and JetStream ready), the C++ trading services check that their
executable is running (`pgrep` on argv[0]; no in-loop heartbeat yet, so this means alive,
not trading correctly), PostgreSQL and the dashboard containers have their own checks.
"Degraded" = running but the check is failing or still starting; "Down" = stopped or unhealthy.

## VPS installation (from scratch, when authorized)

```bash
sudo deploy/chain/algo install /algoTrading
sudo cp /algoTrading/chain/logging/algotrading-logs.service /algoTrading/chain/logging/algotrading-log-cleanup.* /etc/systemd/system/
sudo chown syslog:adm /algoTrading/logs /algoTrading/data/receiver && sudo chmod 0750 /algoTrading/logs
sudo cp /algoTrading/chain/logging/rsyslog.apparmor /etc/apparmor.d/rsyslog.d/algotrading && sudo apparmor_parser -r /etc/apparmor.d/usr.sbin.rsyslogd
sudo systemctl daemon-reload && sudo systemctl enable --now algotrading-logs.service algotrading-log-cleanup.timer
# loads the release's images, installs its chain files and starts (section "Releases"):
sudo /algoTrading/releases/<version>/algo activate /algoTrading/releases/<version> /algoTrading
```

This replaced the previous `deploy/paper_trading` + `deploy/live/logging` installation
under `/opt/algotrading` (removed from the VPS on 2026-10-13).

Chain units (`chain/systemd/`): `algotrading-chain.service` runs `algo up`/`algo down`,
`algotrading-monitor.service` writes `data/telemetry`, `algotrading-comparison.service`
runs the PAPER vs. fast-backtest study (only while `PAPER_COMPARISON_FILE` is set),
`algotrading-kraken.service` runs the Kraken account monitor (below).

## Changing the portfolio sizer of a running chain

The risk checkpoint is bound to the portfolio file it was made with; a different file
stops `risk` ("belongs to a different portfolio/config identity"). To switch on purpose
(for example equal weight -> `pure_rsi_vol_target.json`), away from 00:00 UTC:

```bash
sudo /algoTrading/chain/algo backup
# chain.env: PORTFOLIO_FILE=pure_rsi_vol_target.json and RISK_ADOPT_PORTFOLIO_CONFIG=yes
sudo /algoTrading/chain/algo up                      # risk logs one ALERT "Portfolio config changed on purpose"
# chain.env: RISK_ADOPT_PORTFOLIO_CONFIG=no
sudo /algoTrading/chain/algo up                      # risk restarts with the adopted configuration
sudo systemctl restart algotrading-comparison
```

Earlier decisions keep their sizing; the next daily decision uses the new one (with vol
targeting, open positions are resized when their weight is off by more than 2 points).
`comparison.py` reads `STRATEGY_FILE`/`PORTFOLIO_FILE`, archives the previous study in
`data/comparison/archive-<fingerprint>/` and seeds the fast baseline with the PAPER account
and strategy state at the switch; the rows already shown remain in the chart.

Baseline signals come from the persisted strategy intent at the seed's exact
decision date. Execution snapshots supply cash, holdings and virtual positions;
their empty `signals` field is not the strategy's signal state. This applies to
both PAPER configuration switches and the initial LIVE observation.

An older seeded study without its strategy intent is repaired from that historical
PostgreSQL checkpoint. Before repair, the collector preserves existing study,
manifest, fast and comparison files under `before-seed-repair/`. If the checkpoint
is missing or its date/strategy identities differ, observation stays unavailable;
signals are never inferred from holdings or taken from a later day. The baseline
cache (`fast-input.sha256`) tracks economic inputs/configuration, so a corrected
seed triggers recomputation even when the cycle count is unchanged. These changes
were deployed on 2026-10-14; all five retained rows have zero signal differences.

Telegram severity routing uses optional `DASHBOARD_TELEGRAM_CRITICAL_CHAT_ID` in
`secrets/chain.env`: CRITICAL alerts and their recoveries go to that group; other
messages keep the original chat. INFO/resolved updates are silent; WARN/CRITICAL
use normal notifications, controlled by phone/chat settings. The Kraken notifier
is active in TELEGRAM mode; the general watchdog sink remains TEST_FILE. Explicit
tests use `docker exec kraken-notifier /dashboard-alert-notifier --test-message
--test-severity CRITICAL` (sends a real TEST-labelled message).

## Telegram setup message

Configure `NOTIFIER_SINK=TELEGRAM` in `chain/chain.env` and put
`DASHBOARD_TELEGRAM_BOT_TOKEN` / `DASHBOARD_TELEGRAM_CHAT_ID` in the installation's
private `secrets/chain.env`. Start the notifier with `algo up --no-deps notifier`.
It uses the existing watchdog heartbeat and durable receipt store. An explicit
one-time setup message can be requested while the container is running:

```bash
docker exec notifier /dashboard-alert-notifier --test-message
```

Continuous startup may also send current active warnings. To stop only delivery,
use `algo down notifier`. Earlier ignored Telegram files and receipts are not
deleted by retiring the older deployment. The Kraken account notifier is separate
and uses the `kraken` profile described below.

Both chain notifiers independently warn when their alert source fails continuously
for `DASHBOARD_NOTIFIER_SOURCE_STALE_WARNING_SECONDS` (integer seconds, default 300,
range 1..86400; optional in `chain.env`). The notifier sends one WARN titled
"Alert source stopped" and one resolved notification on recovery, using its existing
sink. Display names come from `DASHBOARD_NOTIFIER_SOURCE_NAME`: `Watchdog` for the
general notifier and `Kraken` for kraken-notifier, set in Compose. The warning states
when failure was first observed and only a safe reason (missing, stale, ahead of
clock, source error). WARN is yellow with normal notification; resolved is green
and silent; both use the normal Telegram chat even with a critical group configured.
The general notifier remains TEST_FILE. Keep each store's `source-health.json`,
decision journal and sink receipts together for restart deduplication and retained
timers. No source alerts are replayed while its evidence is unhealthy. This local
feature requires an authorized deployment; the existing healthcheck is unchanged
and remains unhealthy when source evidence fails.

## Kraken account monitor and Telegram

Warning-only watch of the real Kraken account, independent of PAPER trading:

Notifier source-health contract (`status.json`, step43-v1): `lastSuccessAt` means the
monitor successfully published its own state; `lastError` reports internal monitor
failure. A Kraken API outage is reported separately by the monitor's alert lifecycle
(task 1), while its own heartbeat continues. A missing heartbeat, success older than
90 seconds, success more than 30 seconds ahead or internal error blocks source-alert
delivery and starts the notifier's five-minute source-failure timer described above.
The two changes can be developed independently; deployment should include both.

```text
algotrading-kraken.service (host, python3)          kraken-notifier (container, profile "kraken")
  every 30 s, read-only key:                          reads data/kraken_account
  balances, margin, positions  -> data/kraken_account -> Telegram
  warnings + daily portfolio      events.jsonl, status.json, SQLite journal
```

- Warnings (each sent once, with a "resolved" message when it clears): equity below the
  minimum, USD available below the reserve, initial/maintenance margin, equity down >= 10%
  versus yesterday, gross exposure above 5x, and a fall of less than 25% of BTC and the long
  positions that would reach maintenance margin (`--maintenance-drop-warning` in the unit;
  changing it does not touch the journal). Thresholds: `config/kraken_account_policy.json`
  (installed once from `config/venues/kraken_futures/account_policy.json`).
- One daily portfolio message at 09:00 UTC (balance, equity, BTC collateral before and
  after haircut, maintenance margin and headroom, the price drop that would reach
  maintenance margin, positions in USD).
- `data/kraken_account/account.json` (each successful read): the same collateral and
  margin figures for the dashboard. In `MODE=live` the Overview shows BTC Collateral,
  Sizing Capital (equity + BTC after haircut, what the strategy sizes with), Margin
  Headroom, Drop to Maintenance and Net Deposits; older than 5 minutes it is flagged stale.
  Drop to maintenance is a linear estimate (maintenance margin taken as fixed): "BTC" is
  the fall of BTC alone, "all" the fall of BTC and the net long positions together.
- Kraken not answering (maintenance, HTTP 5xx, network): the cause goes to
  `journalctl -u algotrading-kraken` when it starts, changes and recovers (for example
  "Kraken account read failed: Kraken HTTP 503 at wallets"), never bodies or keys. After
  `--unreadable-warning-minutes` (10) of failed reads one WARN "Kraken account unreadable"
  (KRAKEN_UNREADABLE) goes to Telegram with the cause and the last good read; the next good
  read resolves it. A failed read never changes or resolves the other warnings.
- `status.json` (step43-v1): `lastSuccessAt` is the last sweep in which the monitor itself
  worked and published, whether or not Kraken answered; `lastError` only reports the
  monitor's own failures (journal, disk). A `lastSuccessAt` that stops moving means the
  monitor is stopped. (Before 2026-10-15 a Kraken failure set `lastError`, and the
  notifier then sent nothing.)
- `data/kraken_account/account-log-summary.json` (at start and hourly, retried after 5 min
  on failure): the newest 500 account-history rows without ids (date, info, asset, wallet,
  contract, balances, fee, realized PnL/funding, whether an execution id is present), plus
  every field name with its JSON types and how often each `info` appears. It is for checking
  the gateway's account-history parser; a failure there opens no alert.
- It never places, cancels or closes anything. Raw samples are kept 3 days; alert history
  and per-day portfolio snapshots stay. A failed read never reports a recovery.
- The key must be a Kraken Derivatives key with **General API: Read Only** and
  **Withdrawal API: No Access**; the monitor refuses keys that can trade or move funds.

Enable it on the VPS (only when authorized; the key file is created by you, never shown):

```bash
# 1. Read-only key: exactly KRAKEN_API_KEY=... and KRAKEN_API_SECRET=...
sudo install -m 0600 /dev/null /algoTrading/secrets/kraken_readonly.env && sudo nano /algoTrading/secrets/kraken_readonly.env
# 2. Telegram bot settings in /algoTrading/secrets/chain.env (DASHBOARD_TELEGRAM_BOT_TOKEN, DASHBOARD_TELEGRAM_CHAT_ID)
# 3. chain.env: COMPOSE_PROFILES=kraken   (KRAKEN_NOTIFIER_SINK=TEST_FILE to try without Telegram)
sudo cp /algoTrading/chain/systemd/algotrading-kraken.service /etc/systemd/system/ && sudo systemctl daemon-reload
sudo systemctl enable --now algotrading-kraken
sudo /algoTrading/chain/algo up
sudo /algoTrading/chain/algo ps        # "kraken account monitor: running", kraken-notifier Up
cat /algoTrading/data/kraken_account/status.json   # lastSuccessAt every ~30 s, no lastError
```

Changing the thresholds binds a new journal: stop the unit, move `data/kraken_account`
aside, edit the policy and start again (the old history stays in the moved folder).
Stop: `systemctl disable --now algotrading-kraken` and remove `kraken` from
`COMPOSE_PROFILES`.

## Releases

```bash
deploy/chain/algo build
deploy/chain/algo release storage/releases     # -> storage/releases/<UTC stamp>-<commit>[-dirty]/
```

Each release folder holds:

| File | Content |
|---|---|
| `images.tar` | chain images as `<name>:<version>`, plus nats, postgres, alpine |
| `source.tar.gz` | the source the images came from (no data, builds or credentials) |
| `release.json` | version, commit, image IDs and architecture, `state_format` |
| `algo` | this launcher, so the release can activate itself |
| `SHA256SUMS` | checksums of the four files above |

Deploy on the VPS (only when authorized, outside 23:30-00:30 UTC):

```bash
# from the PC: copy storage/releases/<version>/ to /algoTrading/releases/<version>/ on the VPS
sudo /algoTrading/releases/<version>/algo activate /algoTrading/releases/<version> /algoTrading
sudo /algoTrading/chain/algo releases          # current and previous
sudo /algoTrading/chain/algo rollback          # back to the previous release, same steps
```

`activate` checks before touching anything: checksums, that each image in `images.tar`
has the recorded ID and the host architecture, that `chain.env` runs the `:chain` images,
and that `data/` was not written by a newer state format. Then it stops the chain, makes a
backup (`algo backup`), installs the release's chain files (`algo install`, which never
overwrites config, secrets or data), points `<name>:chain` at the release's images,
records `releases/current`, `releases/previous` and `releases/history.log`, and starts.
`images.tar` is loaded only when its images are not already present, so a rollback is
quick. If a step after the stop fails, the chain stays stopped with the backup on disk.

Not done automatically:

- Host units: `activate` prints a NOTE for every unit in `/etc/systemd/system/` that
  differs from the release; copy it and run `systemctl daemon-reload`.
- Configuration in `/algoTrading/config` is kept as it is. A release that needs a config change says
  so in CURRENT_STATE.md (for example `market_data.json` for the faster 00:00 download).
- Old releases stay in `/algoTrading/releases/` and as `<name>:<version>` images. Remove one with
  `rm -r /algoTrading/releases/<version>` and `docker image rm algotrading-{runtime,dash-api,dash-web}:<version>`;
  keep the current and previous ones.

State format: `STATE_FORMAT` in `algo` is the format of the durable state a release
writes (tables, snapshot JSON, journals); `data/STATE_FORMAT` records the last activated
one. Adding tables or optional fields keeps the number. A release that changes the meaning
of stored state increases it; `activate` then refuses any older release on that data. To go
back anyway, stop the chain and restore the backup taken just before the newer release
(`backups/<stamp>.tar.gz`; extract it into an empty `data/` from a container, like
`algo backup` creates it), then activate the older release. Everything after that backup
is lost, and with real money the venue must be reconciled first.

Releases made before `release.json` (the 2026-10-13 VPS one) carry `:chain` tags and no
image IDs. `activate` accepts them, checked by `SHA256SUMS` only, keeps their images as
`<name>:<version>` and treats them as state format 1. Before the first `activate` on the
VPS, record that release as current so `rollback` can return to it:

```bash
echo <version of the 2026-10-13 release> | sudo tee /algoTrading/releases/current
```

Its own launcher has no `rollback`; to move forward again from it, use the newer
release's copy: `sudo /algoTrading/releases/<newer>/algo activate /algoTrading/releases/<newer> /algoTrading`.

## Moving the VPS PAPER study into the chain (keeps its history)

Historical runbook for the completed 2026-10-13 migration at root `/`. These
commands are not a relocation procedure for the current installation.

`chain/migrate_paper.py` copies the old `algotrading-paper` stack into the installation:
its Docker volumes (PostgreSQL, NATS, market SQLite, watchdog/notifier/audit/alerts),
`storage/paper_trading/comparison`, the Kraken public observation study and
`market_data_config.json`. It reads the old containers' settings (strategy file, RSI
50/40, cash, commission, comparison panel) and writes them to `chain.env`, copies the
dashboard password into `secrets/chain.env` without printing it, gives the copied
PostgreSQL the chain password, and verifies every copied file by SHA-256. It never
changes or deletes the old volumes and folders. Without `--apply` it only checks.

Do it in the middle of the UTC day (far from the 00:00 candle), only when authorized:

```bash
# 1. Release (from the PC): copy storage/releases/<version>/ to /releases/<version>/
cd /releases/<version> && sha256sum -c SHA256SUMS
sudo docker load -i images.tar
sudo mkdir source && sudo tar -xzf source.tar.gz -C source
sudo source/deploy/chain/algo install /

# 2. Stop the old stack (volumes stay)
sudo systemctl stop algotrading-paper-comparison algotrading-paper-monitor algotrading-paper
sudo systemctl disable algotrading-paper-comparison algotrading-paper-monitor algotrading-paper

# 3. Check, then copy
sudo /chain/migrate_paper.py
sudo /chain/migrate_paper.py --apply

# 4. Logs: the new receiver unit has the same name; replace and restart it
sudo cp /chain/logging/algotrading-logs.service /chain/logging/algotrading-log-cleanup.* /etc/systemd/system/
sudo chown syslog:adm /logs /data/receiver && sudo chmod 0750 /logs
sudo cp /chain/logging/rsyslog.apparmor /etc/apparmor.d/rsyslog.d/algotrading && sudo apparmor_parser -r /etc/apparmor.d/usr.sbin.rsyslogd
sudo cp /chain/systemd/*.service /etc/systemd/system/ && sudo systemctl daemon-reload
sudo systemctl restart algotrading-logs.service

# 5. Start
sudo systemctl enable --now algotrading-chain algotrading-monitor algotrading-comparison
sudo /chain/algo ps
```

Check: the dashboard (SSH tunnel, same password) shows the same equity, trades and
positions as before; `logs/<service>/` fills; `data/comparison/comparison.json` refreshes.

Rollback: `systemctl disable --now algotrading-chain algotrading-monitor algotrading-comparison`,
then enable and start the three old units again; their volumes were never touched.

Done on 2026-10-13: the old containers, volumes and images (`algotrading-paper*`, the three
older hashed projects, golang build images), `/opt/algotrading`, `/var/log/algotrading`,
`/etc/algotrading`, the old files in `/home/ubuntu`, the three `algotrading-paper*` units and
the two failed `algotrading-quote-*` units were removed. Everything removed except image
tarballs is in `/backups/old_paper_stack_20261013.tar.gz` (root only, includes the old
`/etc/algotrading` settings): Docker volumes under `var/lib/docker/volumes/`, plus `opt/`,
`etc/`, `var/log/` and `home/ubuntu/`.

## Dirty releases

`algo release` packs `git archive HEAD` for a clean tree. With uncommitted changes the
version ends in `-dirty` and `source.tar.gz` is the working tree instead: tracked files plus
new files git does not ignore (never ignored data or credentials), so the installed chain
files match the images.
