# Current-data paper trading

## Forward activity study and fast baseline

The user authorized a VPS PAPER study with strict PureRSI(7) entry > 50 and exit < 40.
`config/strategies/pure_rsi_paper_activity.json` is a separate experiment; the original
80/70 historical and ordinary PAPER profiles remain intact. Apply
`docker-compose.activity.yml` after the base/VPS/logging files. It selects activity
images and separate NATS/PostgreSQL volumes, while reusing canonical public market
history. Preserve original volumes and back up the stopped original stack before
switching. Keep credentials and the loopback 8092 dashboard binding. The PAPER
systemd unit must include this overlay for start and stop. Removing it selects the
preserved original account/message state again; accounts are not merged.

`comparison.py` captures fully applied, reconciled simulated cycles and frozen SQLite
snapshots in `storage/paper_trading/comparison`. It invokes the existing research fast
binary's explicit PAPER mode in a network-disabled container (384 MiB, 0.4 CPU), using
CURRENT TradingEngine, PureRSI, sizing, constraints and simulated accounting. Only
100-day indicator warmup is read; it never trades warmup days or unfinished closes.
Daily candidate lists and persistent signals are preserved across the captured study.
Missing daily observations, changed profiles or changed captured account state make
comparison unavailable; they do not affect trading or invent historical rows.

Live vs. Expected displays PAPER versus fast equity, cash, signal differences and
positions, marked at the same observed execution open. PAPER resolves quantity from
close(T), whereas fast resolves at open(T+1); gap-driven differences remain visible.
Funding/slippage are not modeled. This is a virtual USD account, not private Kraken
BTC/USD. No real order methods or private keys are used. Each current producer plan
also receives a GET-only Kraken public observation, with evidence/skips separately
under `storage/kraken_shadow/`. Public failures retry without changing simulated orders.

`algotrading-paper-comparison.service` provides automatic capture after startup.
Atomic `comparison.json` is read-only to the API and becomes STALE after three minutes
without refresh. Frozen input, manifest and fast results support restart/review.
The PAPER bundle includes eight service executables plus the research fast binary;
ordinary LIVE packaging is unchanged. Credentials/generated artifacts stay ignored.

This separate stack downloads completed Binance USDT perpetual daily candles,
warms the existing PureRSI signal strategy with 100 days and routes orders through
the existing simulated exchange. It uses virtual funds only, its own PostgreSQL,
SQLite and NATS volumes, and the real-source dashboard. It needs no VPS details or
private exchange credentials. Normal `deploy/live` remains pre-exchange.

New locally built PortfolioRisk service cycles persist sizing, asset/gross cap
reductions, volatility availability and rebalance actions together with each decision.
Risk displays those observations and the cycle configuration fingerprint. HOLD
preserves actual quantity. Older cycles retain unavailable intermediate reports;
this local change does not update the running VPS deployment.

## Local use in WSL

Telegram is optional and remains disabled by default (TEST_FILE notifier). The
[dashboard setup instructions](../../dashboard/README.md#local-telegram-setup)
describe ignored local credentials, an explicit one-message test and starting only
the notifier. Do not update the running VPS while accepting this local work.

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
Trading Services now reports external executable observations from Docker top,
including stopped or missing processes inside otherwise running containers. These
are sampled observations, not in-loop application heartbeats or proof of responsiveness.
Host uptime is labelled separately from trading-process activity.
Clock sync comes from the collector host's systemd NTPSynchronized property: SYNCED,
UNSYNCED or UNKNOWN when unavailable (including this local WSL setup). Clock offset
and network throughput remain unmeasured. The API never receives the Docker socket.

Daily Trading Progress compares persisted decision and plan-application dates with
the current PAPER schedule: completed close yesterday, next-open application today.
It allows 30 minutes after UTC midnight for the new cycle and explicitly reports
missing/behind dates after that grace. Empty/no-order plans count as progress;
recent database writes or fills cannot hide an old business date. Plan application
does not prove fills or reconciliation. This wall-clock policy is not applied to
historical replay or other runtime modes. Older collectors retain UNKNOWN process
evidence. Install the local changes on the VPS only after explicit authorization.

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

## Optional temporary mobile access (cancelled task)

The user cancelled mobile access on 2026-10-09. This retained runbook and unused
helper are optional reference only, not pending acceptance or instructions to
activate access. Keep the current VPS localhost access unchanged.

Keep the dashboard on localhost. `mobile_access.py` runs on the Windows PC and
provides an HTTPS bridge for exactly one phone IP. It forwards the existing login,
role checks, CSRF and event stream unchanged; it does not start trading, change the
VPS or log requests. This is temporary LAN access, not access from outside home.

For the current VPS, first keep this SSH tunnel open in PowerShell:

```powershell
ssh -i "$env:USERPROFILE\.ssh\algotrading-spain.pem" -N -o ExitOnForwardFailure=yes -L 127.0.0.1:8093:127.0.0.1:8092 ubuntu@15.237.145.179
```

The PC can then open `http://localhost:8093`. Use `viewer` and the VPS dashboard
password; the local saved deployment copy is in ignored `storage/vps_deployment/.env`.
Do not send passwords or the SSH key through chat.

Connect the phone to the same trusted Wi-Fi as the PC. Find its IPv4 address in
Wi-Fi settings; use that exact address below. Confirm the PC's Wi-Fi IPv4 address
with `ipconfig` (currently `192.168.1.33`; addresses can change). In a second
PowerShell terminal:

```powershell
$pcIp = '192.168.1.33'
$phoneIp = 'REPLACE_WITH_PHONE_WIFI_IPV4'
python deploy/paper_trading/mobile_access.py --bind-ip $pcIp --phone-ip $phoneIp --target-port 8093
```

For a running local PAPER dashboard, use `--target-port 8092` instead; use the local
viewer credentials from `storage/paper_trading/.env`. The bridge fails before
listening if its upstream is unavailable. Python 3 and OpenSSL are required; it
finds OpenSSL on PATH or uses the installed Git for Windows copy.

If Windows Firewall blocks the phone, add a temporary rule in **administrator
PowerShell**, with the same verified addresses, on this trusted Wi-Fi only:

```powershell
New-NetFirewallRule -Name 'AlgoTradingMobileTemporary' -DisplayName 'AlgoTrading temporary phone access' -Direction Inbound -Action Allow -Protocol TCP -LocalPort 8094 -LocalAddress $pcIp -RemoteAddress $phoneIp -Profile Any
```

This rule uses both exact IP addresses because the current Wi-Fi profile is Public;
it does not change the network profile or allow every LAN peer. Do not forward any
router ports or change Lightsail firewall rules. The bridge itself also rejects
other source IPs before TLS. A phone using mobile data cannot reach this LAN URL.

Open `https://192.168.1.33:8094` on the phone, using the current PC IP. The generated
certificate is self-signed, so the browser may require a certificate exception or
explicit installation/trust. Verify its SHA-256 fingerprint against the bridge's
terminal output before trusting it; never copy `private-key.pem` to the phone.
Certificates/keys stay in ignored `storage/mobile_access/<PC-IP>/` and expire after
30 days. Renew using a fresh certificate directory while the bridge is stopped.
Do not fall back to unencrypted HTTP for phone login. This bridge does not rewrite
the upstream cookie flags; its mobile listener accepts TLS only.

Acceptance requires actual phone login, opening Overview/Pipeline/Risk/Infrastructure,
receiving fresh stream updates, logging out and confirming private pages require
login again. Transport fixture tests do not replace this handset check.
Ctrl+C stops the bridge; close the SSH terminal to stop the tunnel. Remove the
temporary firewall rule in administrator PowerShell:

```powershell
Remove-NetFirewallRule -Name 'AlgoTradingMobileTemporary'
```

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

## Local backup and recovery rehearsal

With current locally built packaged images, run under WSL:

```bash
PAPER_RECOVERY_TEST=1 python3 validation/paper_trading_test.py
```

Optional `PAPER_RUNTIME_IMAGE`, `PAPER_API_IMAGE` and `PAPER_WEB_IMAGE` select current
tags without changing normal PAPER images. The random fixture uses its own state
and forces TEST_FILE notifications. It verifies trading with all dashboard services
stopped, HTTP/NATS/PostgreSQL outages, controlled recovery and a fresh-volume restore.
Its evidence and backup manifest are under ignored `storage/paper_validation/`.

The tested backup boundary is a maintenance stop: quiesce every trading and observer
writer, confirm a drained exchange outbox, stop NATS, then take a PostgreSQL custom
logical dump and archive the stopped market/NATS/observer volumes. A stopped market
volume includes its SQLite database and any WAL/SHM files; never copy only a live
SQLite `.db`. Never copy a running PostgreSQL data directory. Verify archive/dump
checksums before mutation, restore into fresh volumes, compare restored contents,
then start with matching images/settings and verify retained fills/account state.

Credentials are stored separately; the fixture does not include `.env` in its backup.
This rehearsal reuses its existing controlled configuration and images. A production
recovery plan must additionally preserve their versions, compatible schemas, secure
credentials and off-host backups, define retention and verify restoration on the VPS.
It does not authorize changing or stopping the running VPS, overwriting existing
volumes, or assuming every service reconnects automatically after a database fault.
