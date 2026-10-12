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

This replaces the previous `deploy/paper_trading` + `deploy/live/logging` installation
under `/opt/algotrading`, which the running VPS still uses until it is redeployed.

Chain units (`chain/systemd/`): `algotrading-chain.service` runs `algo up`/`algo down`,
`algotrading-monitor.service` writes `data/telemetry`, `algotrading-comparison.service`
runs the PAPER vs. fast-backtest study (only while `PAPER_COMPARISON_FILE` is set).

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

After some accepted days: delete the old containers and volumes (`algotrading-paper*`
and the three older hashed projects), `/opt/algotrading`, `/var/log/algotrading`,
`/etc/algotrading`, the old archives in `/home/ubuntu` and the two failed
`algotrading-quote-*` units. Keep one backup in `/backups` first.
