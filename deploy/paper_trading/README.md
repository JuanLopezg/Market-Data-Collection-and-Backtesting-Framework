# Current-data paper trading

This separate stack downloads completed Binance USDT perpetual daily candles,
warms the existing PureRSI signal strategy with 100 days and routes orders through
the existing simulated exchange. It uses virtual funds only, its own PostgreSQL,
SQLite and NATS volumes, and the real-source dashboard. It needs no VPS details or
private exchange credentials. Normal `deploy/live` remains pre-exchange.

## Local use in WSL

From the repository root, prepare and build once:

```bash
python3 deploy/paper_trading/manage.py prepare
python3 deploy/paper_trading/manage.py build
```

Then one command starts the services and host monitor:

```bash
python3 deploy/paper_trading/manage.py run
```

Open **http://localhost:8092**, log in as `viewer` using
`DASHBOARD_VIEWER_PASSWORD` from ignored `storage/paper_trading/.env`, and select
**Infrastructure**. CPU, RAM, disk capacity/free space and container health/resource
use refresh every 10 seconds. The dashboard does not get the Docker socket.
Snapshots older than 35 seconds are unavailable; missing measurements are not zero.
Host CPU is a percentage of total capacity; container CPU 100% means one CPU core.
On Docker Desktop, WSL host measurements and container measurements describe
different Linux environments; neither is a measurement of total Windows resources.
Container running/health checks do not establish trading readiness. Clock sync and
network throughput remain explicitly unmeasured.

Keep the terminal open. Ctrl+C stops the monitor and stack, preserving durable
volumes. `start`/`stop` operate containers separately; `monitor` runs observation
and local daily log collection; `status` displays container state. No command here
removes volumes. Public data requires Internet access; failures retry without
publishing a completed trading cycle.

Runtime configuration and credentials live under ignored `storage/paper_trading`.
The default is 100,000 virtual cash and commission rate 0.001 (0.1%). The ingestion
universe starts with the current 24-hour quote-volume ranking, maintaining its
existing retained symbols. This is not the pending 25-day exchange coverage study.

PAPER's default strategy is `config/strategies/pure_rsi_quote_volume.json`: top 20
by SMA(QuoteVolume,25) inside that top-50 candidate set. `quote_volume` is Binance's
actual completed-kline turnover in USDT (field 7), not base units or `volume * close`.
The downloader adds the nullable SQLite column and backfills missing quote values
within the configured warmup window. Unfinished quote turnover never reaches strategy.
Existing `volume` values remain unchanged for historical replay and execution capacity.
The dashboard labels the metric explicitly; missing required quote rows are integrity
issues, and Strategy refuses to turn them silently into flat signals.

For an existing deployment, preserve all durable checkpoints and account state,
back up both databases, and migrate the guarded strategy configuration identity
only after verifying the intended change and current exposure. Set `QUOTE_VOLUME_FROM`
to the first future completed candle using the new selection. Dashboard signals from
older cycles remain unaligned, and its rolling baseline excludes pre-cutover cycles.
Do not clear checkpoints to rerun a completed day. The original
`pure_rsi_signal.json` profile remains available for historical source-volume validation;
an explicit legacy PAPER override must also set `QUOTE_VOLUME=false` for the dashboard.

When deploying source snapshots, preserve useful file timestamps or build in a fresh
directory. After a shared bar layout changes, rebuild every runtime target from a
clean build before packaging. A matching source manifest does not prove that an
incremental build rebuilt all affected objects. The ingestion-to-fill fixture must
exercise the actual packaged binaries before activating an upgrade.

## Simulation boundaries

The downloader keeps only completed bars through decision day T in canonical
SQLite. An opt-in query extracts **only open(T+1)** from the already-started daily
candle and publishes the existing execution-price contract before the market
update. Unfinished high/low/close/volume never become strategy inputs. Ordinary
LIVE configuration leaves this publisher disabled.

The simulated exchange fills at that opening price even when startup/catch-up
happens later in the day. It therefore validates operational wiring, not achievable
live fill prices. Monetary orders retain the existing service conversion using
close(T), with execution at open(T+1); this is not a canonical next-open sizing
parity claim. Virtual cash/full-fill accounting does not model production perpetual
collateral, funding, liquidation or realistic latency/slippage. Those remain roadmap
work before private trading. Only the simulated venue/environment/backend combination
is accepted by the dashboard's PAPER configuration.

## Logging and VPS handoff

Local `run`/`monitor` captures all 13 long-running services once per minute into
`storage/paper_trading/algotrading/services`. It uses the existing writer: today and
yesterday in UTC, at most 50 MiB per daily file, trimming older diagnostics to make
room. Credentials in diagnostics are redacted. Docker's local fallback cache is
bounded to two 25-MB files per container; it is not the durable daily archive.
Without the local collector or a VPS receiver, daily capture is not active.

On a Linux VPS, install the existing receiver/cleanup units described in
[the logging guide](../live/README.md#vps-log-retention), then add
`docker-compose.logging.yml` last to this stack's Compose command. It covers every
service, including the simulated backend, watchdog and notifier. Install the
provided `algotrading-paper-monitor.service` after placing the repository at
`/opt/algotrading` and creating the telemetry directory. The host monitor is a
privileged observer because Docker access is privileged; dashboard containers
remain isolated from that socket. The notifier uses its local test sink, not Telegram.

No VPS units are installed by the local commands. Actual VPS UTC rollover, trimming,
receiver restart/recreation, fault recovery, overnight growth and capacity acceptance
remain pending. Transfer source/config and the freshly built paper runtime bundle;
keep research collections, build caches, local diagnostic evidence and credentials
out of the handoff. Prepare fresh credentials on the VPS and verify matching build,
settings and Step59 output before starting it. Binding the dashboard to loopback is
intentional; use SSH forwarding for remote viewing.

## Persistent VPS operation

Place the current source at `/opt/algotrading`, build on the destination host and
pass the bounded Step59 gate before starting PAPER. Prepare fresh credentials with
`manage.py prepare`; do not copy the local account/database or `.env`. On a 2-GiB
host, compile with one C++ compiler process (`meson compile -C build -j 1 ...`).
Build Compose targets separately: `market-data-db-init`, then `dashboard-api`, then
`dashboard-web`. Workers share those images. Compose's global parallel setting
does not necessarily serialize BuildKit stages across requested images. Swap is a
reserve for transient peaks, not evidence that the plan has sufficient capacity.

Install the host logging units first, then install
`algotrading-paper.service` and `algotrading-paper-monitor.service` into
`/etc/systemd/system/`. After `systemctl daemon-reload`, enable/start both paper
units. The paper unit applies the base Compose file, `docker-compose.vps.yml`,
then the logging overlay. The VPS overlay adds restart policies for NATS,
PostgreSQL and dashboard API; workers already have them. The unit starts after
Docker and the log receiver, and stopping it preserves volumes.

```bash
sudo systemctl enable --now algotrading-paper.service algotrading-paper-monitor.service
sudo systemctl status algotrading-paper.service algotrading-paper-monitor.service
sudo systemctl stop algotrading-paper.service algotrading-paper-monitor.service
```

For remote viewing, keep port 8092 bound to loopback. From the client computer:

```powershell
ssh -i "$env:USERPROFILE\.ssh\algotrading-spain.pem" -N -L 8093:127.0.0.1:8092 ubuntu@VPS_IP
```

Open `http://127.0.0.1:8093` while that connection is open. Port 8093 distinguishes
the VPS dashboard from the local PAPER dashboard on 8092. Log in as `viewer` using
the VPS `storage/paper_trading/.env`, not the local deployment's password. The SSH
key stays outside the repository. Real reboot, overnight retention and fault
acceptance must be recorded separately from installing/enabling these units.

## Bounded validation

```bash
python3 validation/paper_trading_test.py
bash validation/step59_canonical_replay_release_gate.sh
```

The first test uses the actual ingestion binary with a controlled public-API-shaped
fixture on an internal Docker network. It validates completed-bar visibility,
execution-only opening prices, fills/cash/positions, aligned checkpoints, dashboard
telemetry, stale collector handling and restart without duplicate fills. It stops
only its random test project and retains ignored evidence/volumes. It never calls
a private venue, alters host time, runs full history or waits overnight.
