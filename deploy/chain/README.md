# Chain: runtime layout and launcher

One installation per machine. Every runtime file lives under one root folder:
`/` on the VPS, any Linux folder locally (for example `~/algotrading-runtime` in WSL;
not `/mnt/c`, because PostgreSQL needs real file ownership).

```text
<root>/
├── chain/      compose.yml, algo, chain.env, monitor.py, logging/   (what you operate)
├── config/     non-secret configuration: strategies/, portfolio/, market_data.json
├── secrets/    chain.env with credentials (0700/0600; never print or copy it around)
├── data/       durable state, one folder per owner
│   ├── postgres/    orders, accounts, checkpoints
│   ├── nats/        JetStream messages
│   ├── market/      database.db (candles, quote volume)
│   ├── dashboard/   watchdog/, audit/, alerts/, notifier/
│   ├── telemetry/   host.json from monitor.py (regenerated, not backed up)
│   └── receiver/    log receiver state (not backed up)
├── logs/       <service>/YYYYMMDD.log, today and yesterday, at most 50 MiB per file
├── reports/    generated outputs to read
├── backups/    YYYYMMDD_HHMMSS.tar.gz of data/
└── releases/   image archives per version (rollback)
```

Only `data/`, `config/` and `secrets/` must survive a reinstall. `chain/` is replaced
by `algo install`; `logs/` and `reports/` can be deleted.

`reports/` (written by `dash-api` every 10 minutes, rebuilt from all persisted fills):
`closed_positions.csv` and `closed_positions_<YEAR>.csv` (by UTC close year), one row per
position from flat to flat: asset, side, opened/closed, quantity, entry/exit price, cost
basis, proceeds, gross PnL, fees, funding (+ received) and net PnL = gross - fees +
funding, in USD. Use them for the yearly tax report and to match the exchange export.
Funding is not persisted by the PAPER chain yet, so it is 0 there. Files are not written
when the fill history does not fit one read (200,000 fills).

## Services

| Service | Role |
| --- | --- |
| `nats`, `postgres` | messaging and durable state |
| `market-data` | Binance public candles into `data/market/database.db` |
| `strategy` | PureRSI intents from completed candles |
| `risk` | portfolio risk and target weights |
| `planner` | notional order plans |
| `execution` | order state, fills and accounting |
| `gateway` | exchange command/backend boundary |
| `sim-exchange` | PAPER venue: virtual account and simulated fills |
| `dash-api`, `dash-web` | dashboard API and web on `127.0.0.1:DASHBOARD_PORT` |
| `watchdog`, `notifier` | alert evaluation and delivery (test file or Telegram) |
| `data-init`, `market-data-init` | one-shot folder ownership and market schema setup |

The container name equals the service name, so logs land in `logs/<service>/`.
Each line is exactly what the service wrote:
`YYYY-MM-DD HH:MM:SS.mmm [LEVEL] Class::function : message` (UTC).

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
viewer password is in `secrets/chain.env`.

## Settings

`chain/chain.env` (non-secret): images, `STRATEGY_FILE`, `PORTFOLIO_FILE`, the RSI
values shown by the dashboard (keep them equal to the strategy file), initial cash,
commission, warmup days, dashboard port and notifier sink.

`secrets/chain.env`: PostgreSQL and dashboard passwords, optional Telegram bot settings.

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

## VPS installation (from scratch, when authorized)

```bash
sudo deploy/chain/algo install /
sudo cp /chain/logging/algotrading-logs.service /chain/logging/algotrading-log-cleanup.* /etc/systemd/system/
sudo chown syslog:adm /logs /data/receiver && sudo chmod 0750 /logs
sudo cp /chain/logging/rsyslog.apparmor /etc/apparmor.d/rsyslog.d/algotrading && sudo apparmor_parser -r /etc/apparmor.d/usr.sbin.rsyslogd
sudo systemctl daemon-reload && sudo systemctl enable --now algotrading-logs.service algotrading-log-cleanup.timer
# load released images (docker load < /releases/<version>/images.tar), then:
sudo /chain/algo up
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
sudo /chain/algo backup
# chain.env: PORTFOLIO_FILE=pure_rsi_vol_target.json and RISK_ADOPT_PORTFOLIO_CONFIG=yes
sudo /chain/algo up                      # risk logs one ALERT "Portfolio config changed on purpose"
# chain.env: RISK_ADOPT_PORTFOLIO_CONFIG=no
sudo /chain/algo up                      # risk restarts with the adopted configuration
sudo systemctl restart algotrading-comparison
```

Earlier decisions keep their sizing; the next daily decision uses the new one (with vol
targeting, open positions are resized when their weight is off by more than 2 points).
`comparison.py` reads `STRATEGY_FILE`/`PORTFOLIO_FILE`, archives the previous study in
`data/comparison/archive-<fingerprint>/` and seeds the fast baseline with the PAPER account
and strategy state at the switch; the rows already shown remain in the chart.

## Kraken account monitor and Telegram

Warning-only watch of the real Kraken account, independent of PAPER trading:

```text
algotrading-kraken.service (host, python3)          kraken-notifier (container, profile "kraken")
  every 30 s, read-only key:                          reads data/kraken_account
  balances, margin, positions  -> data/kraken_account -> Telegram
  warnings + daily portfolio      events.jsonl, status.json, SQLite journal
```

- Warnings (each sent once, with a "resolved" message when it clears): equity below the
  minimum, USD available below the reserve, initial/maintenance margin, equity down >= 10%
  versus yesterday, gross exposure above 5x. Thresholds: `config/kraken_account_policy.json`
  (installed once from `config/venues/kraken_futures/account_policy.json`).
- One daily portfolio message at 09:00 UTC (balance, equity, positions in USD).
- It never places, cancels or closes anything. Raw samples are kept 3 days; alert history
  and per-day portfolio snapshots stay. A failed read never reports a recovery.
- The key must be a Kraken Derivatives key with **General API: Read Only** and
  **Withdrawal API: No Access**; the monitor refuses keys that can trade or move funds.

Enable it on the VPS (only when authorized; the key file is created by you, never shown):

```bash
# 1. Read-only key: exactly KRAKEN_API_KEY=... and KRAKEN_API_SECRET=...
sudo install -m 0600 /dev/null /secrets/kraken_readonly.env && sudo nano /secrets/kraken_readonly.env
# 2. Telegram bot settings in /secrets/chain.env (DASHBOARD_TELEGRAM_BOT_TOKEN, DASHBOARD_TELEGRAM_CHAT_ID)
# 3. chain.env: COMPOSE_PROFILES=kraken   (KRAKEN_NOTIFIER_SINK=TEST_FILE to try without Telegram)
sudo cp /chain/systemd/algotrading-kraken.service /etc/systemd/system/ && sudo systemctl daemon-reload
sudo systemctl enable --now algotrading-kraken
sudo /chain/algo up
sudo /chain/algo ps        # "kraken account monitor: running", kraken-notifier Up
cat /data/kraken_account/status.json   # lastSuccessAt every ~30 s, no lastError
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

Each release folder holds `images.tar` (chain images plus nats, postgres, alpine),
`source.tar.gz` (no data, builds or credentials) and `SHA256SUMS`. Copy it to
`/releases/` on the VPS; the previous release folder is the rollback.

## Moving the VPS PAPER study into the chain (keeps its history)

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
