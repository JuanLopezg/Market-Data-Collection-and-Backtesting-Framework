# LIVE pre-exchange deployment

This directory is the production-path deployment scaffold after the fake/replay clock cleanup.
The former shared-clock `deploy/distributed_replay/` topology has been removed.
Run commands below from the repository root under WSL.

The default topology starts only the exchange-agnostic LIVE path:

`MarketData -> Strategy -> PortfolioRisk -> ExecutionState -> OrderPlanner`

It does **not** start replay-controller or simulated-exchange, and it does **not** submit real exchange orders.
`exchange-gateway` is packaged but is behind the optional Compose profile `exchange-edge` while private exchange integration remains unfinished.

## One-time preparation

Copy the environment template and set a real PostgreSQL password:

```bash
cp deploy/live/.env.example deploy/live/.env
# edit deploy/live/.env
```

Make sure the host market-data directory exists:

```bash
mkdir -p storage/databases
```

For a brand-new database, initialize the canonical market DB before starting all read-only consumers:

```bash
# Build first (see below), then bring up infrastructure only.
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml up -d nats postgres

# Run exactly one market-data update. This creates/updates the SQLite schema and
# commits the previous completed UTC day before publishing market.data.updated.v1.
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml run --rm \
  market-data algotrading_market_data_service \
  --config /opt/algotrading/live/market_data_config.json --run-once
```

If `storage/databases/database.db` already contains the canonical schema, the bootstrap command is optional.

## Build the LIVE runtime image inputs

From the repository root:

```bash
meson compile -C build
bash deploy/live/build_runtime_bundle.sh
```

The generated `.runtime_bundle/` contains only the six LIVE binaries:

- market-data
- strategy
- portfolio-risk
- order-planner
- execution-state
- exchange-gateway

It deliberately excludes replay-controller and simulated-exchange.

## Run the pre-exchange LIVE pipeline

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml up --build -d
```

At this stage the acceptance target ends at `NotionalOrderPlan`. No real venue order is sent.

To inspect logs:

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml logs -f \
  market-data strategy portfolio-risk execution-state order-planner
```

Stop without deleting durable PostgreSQL/JetStream state:

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml down
```

Do not use `down -v` unless you intentionally want to delete the LIVE Docker volumes.

## Exchange gateway profile

The gateway is not part of the default pre-exchange run. It can be started explicitly for transport-only testing:

```bash
docker compose --env-file deploy/live/.env -f deploy/live/docker-compose.yml \
  --profile exchange-edge up -d exchange-gateway
```

This profile starts the gateway transport service only; it does not add a private
venue implementation or enable real submit/cancel/fill lifecycle. Dashboard public
TESTNET metadata/dry-run configuration is separate evidence, not a connected backend.

## Validate the pre-exchange boundary

Local adapter crash/restart integration can be checked without deploying LIVE:

```bash
python3 validation/transport_persistence_integration_test.py
```

This uses isolated test NATS/PostgreSQL containers and the actual library adapters
to exercise transactional rollback, durable redelivery after commit-before-ACK,
deduplication and broker/database restart. It does not run the complete service
pipeline or accept a deployed VPS. Dependencies and exact limits are in the
[validation guide](../../validation/README.md).

Run the current structural gate:

```bash
bash validation/live_pre_exchange_e2e_audit.sh .
```

After a real daily cycle `T` has propagated through the running Compose topology, run:

```bash
bash validation/live_pre_exchange_runtime_acceptance.sh . YYYYMMDD
```

The verifier checks the same `YYYYMMDD` in durable MarketData/Strategy,
AccountSnapshot, Risk decision and Planner request/plan evidence, exactly one checkpoint
per stage, and unchanged reference-close maps. Its source is
`validation/live_pre_exchange_runtime_acceptance.sh`.

The acceptance boundary remains `NotionalOrderPlan`. This verifier does not
perform a complete restart/duplicate/broker-failure campaign. Canonical replay restart
is tested separately by Step59; deployed service recovery remains its own acceptance
work. See [CURRENT_STATE.md](../../CURRENT_STATE.md) and
[validation/README.md](../../validation/README.md).

## Local-to-VPS equivalence acceptance

Run this comparison before starting or recreating VPS trading containers. Actual
VPS access and its repository path are required; local success alone cannot accept
the VPS. Preserve the current working tree when transferring source: this checkout
contains uncommitted research/library changes, so a checkout of HEAD is not the
same baseline. Transfer credentials separately and do not copy mutable databases,
journals or Docker volumes over an existing deployment.

On both the local WSL checkout and the Linux VPS checkout, from the repository root:

```bash
mkdir -p storage
python3 tools/generate_ai_index.py
python3 tools/generate_ai_index.py --check
meson introspect --compilers build > storage/vps_equivalence_compilers.json
meson introspect --buildoptions build > storage/vps_equivalence_buildoptions.json
sha256sum deploy/historical_replay/run/1d_cmc_by_date.csv \
  storage/backtests/final_tests/pureRSI.csv \
  config/historical_replay/step56a_source_symbol_map_v1.csv \
  > storage/vps_equivalence_inputs.txt
meson compile -C build -j 3 > storage/vps_equivalence_build.log 2>&1
bash deploy/live/build_runtime_bundle.sh > storage/vps_equivalence_bundle.log 2>&1
bash validation/live_deploy_topology_audit.sh . > storage/vps_equivalence_topology.log 2>&1
bash validation/step59_canonical_replay_release_gate.sh > storage/vps_equivalence_step59.log 2>&1
```

For a fresh Linux build, configure with `meson setup build` first after installing
the project dependencies. Required replay inputs include the local dataset excluded
from a fresh Git checkout; transfer it explicitly and verify its checksum.
Compare the generated `.ai/index.json` source inventory and the three input checksums
between hosts. The index deliberately excludes private/generated data and is not a
complete deployment manifest; also compare `deploy/live/market_data_config.json`
and the selected strategy/portfolio configuration files. Record architecture, OS,
the configured compiler/build options, Meson, Docker/Compose versions and the
built image ID on each host. Use Meson introspection rather than assuming the
shell's default compiler is the configured build compiler.
Rebuild the runtime bundle on the destination host instead of assuming WSL binaries
or binary hashes are portable across architectures/toolchains.

Both Step59 logs must report the accepted fingerprint, 25/25 matched trades, zero
differences, 50 fills and all restart/dashboard/pacing checks. A mismatch must be
explained before deployment. Step59 is a bounded canonical proof, not evidence
that deployed NATS/PostgreSQL services or a private venue work.

Review effective configuration without saving or printing a credential-bearing
`docker compose config` dump. Verify the intended strategy, portfolio, market-data
path, warmup/history settings, loopback port bindings and logging overlay; retain
only non-secret settings. After the runtime image is built on the target, check
all six packaged binaries with `--help` in one-off containers without service
networks or data mounts to catch missing shared libraries. This is a packaging
smoke check, not a pipeline acceptance. Use a separate image tag so verification
does not replace the image selected by an existing deployment:

```bash
docker build --pull=false -t algotrading-runtime:vps-baseline deploy/live/.runtime_bundle
docker image inspect algotrading-runtime:vps-baseline --format '{{.Id}}'
for binary in deploy/live/.runtime_bundle/bin/*; do
  docker run --rm --network none --read-only --cap-drop ALL \
    --security-opt no-new-privileges \
    --entrypoint "/opt/algotrading/bin/$(basename "$binary")" \
    algotrading-runtime:vps-baseline --help || break
done
```

Require a successful exit from every one of the six binaries; a truncated loop
or a loader error is a failed packaging check.

Actual deployment follows the existing LIVE instructions and daily logging setup.
Once a completed daily cycle has propagated, use the runtime acceptance command
above and retain its evidence. VPS equivalence remains pending until the source,
inputs, build, configuration, image and deployed-cycle checks have been observed
on the VPS. Keep all captures under ignored `storage/`; never archive secrets.

## VPS disk capacity and transfer scope

The local disk inventory is recorded in CURRENT_STATE.md. For an initial 60 GB
Linux VPS, use the following **planning reserves**, not configured storage caps:

| Category | Reserve |
| --- | ---: |
| Linux, Docker engine and host packages | 8 GiB |
| Selected runtime/dashboard/infrastructure images and previous release | 3 GiB |
| Source/configuration, runtime bundle and bounded validation inputs | 1 GiB |
| Market SQLite, PostgreSQL/WAL, JetStream and dashboard durable stores | 5 GiB |
| Two-day host diagnostic archive (up to 15 paper service/init identities) | 1.5 GiB |
| Docker inspection log caches | 1 GiB |
| Temporary backup/restore staging | 3 GiB |
| Update/download temporary space | 2 GiB |
| **Total planning allocation** | **24.5 GiB** |

If the advertised 60 GB is decimal, it is about 55.9 GiB before filesystem
overhead, leaving roughly 31 GiB beyond this allocation. Verify actual capacity
with `df -h /` and `df -h /var/lib/docker` on the VPS. These reserves are estimates;
they do not establish a sustained growth rate or production capacity acceptance.

Transfer the current source/configuration and explicitly required bounded replay
inputs, preserving uncommitted work. The canonical input CSV and RealTest reference
are needed for Step59, but the research parquet/intraday collection is not an input
to the default LIVE topology. Do not copy all of `storage/`, local campaign volumes,
Docker Desktop data/build cache, research reports, `.ai/`, dependency caches or
local build directories. Rebuild for the destination toolchain as specified above.
Transfer credentials separately. Initialize fresh VPS state or restore a separately
verified backup; never copy a running SQLite `.db` alone while WAL is active, or
overwrite an existing trading deployment with local fixture state.

With the daily logging overlays, eight LIVE services including the optional gateway
have nominal 20m x 5 Docker caches; API/web have 10m x 3 caches and watchdog/notifier
retain their production 10m x 3 JSON logs. Allow approximately 920 MB for these
configured per-container file budgets, plus rotation/metadata overhead. The default
seven-service LIVE topology uses one fewer 100 MB cache. This applies to the selected
overlays; the base LIVE NATS/PostgreSQL logging is not explicitly size-bounded.
The current dashboard daily overlay forwards API/web only. Include watchdog/notifier
in the daily archive when preparing the VPS configuration so all service logs meet
the persistent two-day requirement; their present production JSON logs are size-only.

The host archive retains today and yesterday, with a 50 MiB cap per daily file.
The writer trims older entries before appending past the cap, preserving about
25 MiB of the newest complete lines and adding a visible `LOG-TRIM` marker.
There are at most two dated files per stable service identity after cleanup, without
`.1`/compressed archives; a quiet service may have fewer. The original 12-service
budget was 1,200 MiB. The separate paper stack adds the simulated backend and
two initializer identities: at most 1,500 MiB, covered by the updated 1.5 GiB reserve.
Its daily overlay also covers watchdog/notifier; all 15 Docker caches are bounded to
two 25-MB files (750 MB nominal total). Extra/recreated service identities need an
adjusted budget. Actual VPS
acceptance remains necessary rather than extrapolating the short captured Compose log.
JetStream stream creation sets file storage but no explicit age/byte cap;
PostgreSQL fills and dashboard audit/alert/notification journals accumulate.
Market-data `retain_after_top_n_days` limits download eligibility, not deletion of
stored OHLCV. Do not apply diagnostic cleanup to trading state or change durable
retention without validating replay/recovery requirements.

Before accepting the VPS, record total/free filesystem space, selected image usage,
named-volume usage, SQLite/WAL sizes and host log bytes at the start and end of the
overnight campaign. Use `docker system df -v`, `du` and `df`; do not prune resources
as part of measurement. Keep at least 15 GiB free as an initial operational target
and include disk-growth/free-space alerts in the infrastructure work. Keep long-term
backups off-host; the staging reserve is not a backup retention policy.

## VPS log retention

The host receiver and cleanup units are implemented in `logging/`. The optional
`docker-compose.logging.yml` overlay forwards all eight LIVE services' stdout/stderr,
including NATS/PostgreSQL and the optional gateway, to host-local TCP port 5514.
It forces stdout logging and disables the C++ services' optional unbounded file
output. Trading commands, profiles, mounts and execution behavior remain unchanged.
The base Compose file retains its existing local logging.

The host archive is `/var/log/algotrading/services/algotrading.<container-name>/YYYY-MM-DD.log`.
Dates and record timestamps use UTC receipt time, independent of trading business
time. Original event time, service identity and severity remain in each record.
Container recreation reuses the named service directory; receiver restart appends
to existing files. Rsyslog filters/formats records and passes them through `omprog`
to `clean_daily_logs.py --receive`, which owns append/trim ordering under a file lock.
The same script's default mode expires and bounds quiet files from the timer.
Files are mode 0640 and directories 0750. Known credential
assignments, credential-bearing URLs and key material are replaced by a redaction
marker. Services must still never emit secrets: Docker's cache/source console
messages are upstream of this defensive archive filter.

The persistent timer runs every minute, removing only correctly named diagnostic
files older than yesterday, including quiet services. The writer also expires old
files on startup and on a UTC date change. Quiet-service expiry at midnight can lag
until the next timer run. Symlinks, unknown files, journals, fills and checkpoints
are preserved. The 50 MiB limit is checked before each append; the writer keeps the
newest complete lines within roughly half the limit, records a `LOG-TRIM` marker and
continues in the same dated file. This deliberately discards older diagnostics.
No numbered backup files are created. A single oversized diagnostic is replaced
with an omission marker. Docker's cache remains separately size-bounded for
`docker compose logs`. A 4 MB non-blocking buffer avoids stalling trading on receiver
failure; prolonged outages/buffer exhaustion can lose forwarded diagnostics. This
archive is not a durable trading journal or an end-to-end delivery guarantee.

### Install on the Linux VPS

Use Docker Compose 2.24.4+ (`!override` replaces the old driver's options), rsyslog 8
with its `omprog` module (locally tested on 8.2112.0), and Python 3.9+.
Run from the repository root on the actual Linux Docker host;
Docker Desktop's daemon loopback is different from WSL's loopback. These commands
prepare host logging without deploying trading services:

```bash
sudo apt-get install rsyslog python3
sudo install -d -m 0755 /etc/algotrading/logging /usr/local/lib/algotrading
sudo install -m 0644 deploy/live/logging/rsyslog.conf /etc/algotrading/logging/rsyslog.conf
sudo install -m 0644 deploy/live/logging/clean_daily_logs.py /usr/local/lib/algotrading/clean_daily_logs.py
sudo install -m 0644 deploy/live/logging/algotrading-logs.service /etc/systemd/system/
sudo install -m 0644 deploy/live/logging/algotrading-log-cleanup.service /etc/systemd/system/
sudo install -m 0644 deploy/live/logging/algotrading-log-cleanup.timer /etc/systemd/system/

# Ubuntu 24.04 confines rsyslogd with AppArmor. Keep that profile enabled and
# permit only the dedicated receiver paths and its bounded Python writer.
if [ -f /etc/apparmor.d/usr.sbin.rsyslogd ]; then
  sudo install -d -m 0755 /etc/apparmor.d/rsyslog.d
  sudo install -m 0644 deploy/live/logging/rsyslog.apparmor /etc/apparmor.d/rsyslog.d/algotrading
  sudo apparmor_parser -r /etc/apparmor.d/usr.sbin.rsyslogd
fi
sudo systemctl daemon-reload
sudo systemctl enable --now algotrading-logs.service algotrading-log-cleanup.timer
sudo systemctl start algotrading-log-cleanup.service
sudo systemctl status algotrading-logs.service algotrading-log-cleanup.timer
sudo ss -ltn 'sport = :5514'
```

The Ubuntu rsyslog package supplies the `syslog` user and `adm` group; systemd creates
the receiver's runtime/state/log directories. Only loopback is bound, with no public
port or Docker-socket mount. Start the receiver before recreating containers with
the overlay. When deploying, add the logging overlay last:

```bash
docker compose --env-file deploy/live/.env \
  -f deploy/live/docker-compose.yml -f deploy/live/docker-compose.logging.yml up -d
```

For the dashboard, add `dashboard/docker-compose.logging.yml` last after its existing
production and real-provider files, using the same receiver. Logging settings apply
to recreated containers. Do not change a running VPS during local validation.

### Validation and outstanding VPS acceptance

Under WSL, without deploying the LIVE stack:

```bash
python3 validation/daily_service_logs_test.py
bash validation/live_deploy_topology_audit.sh .
```

Fixtures use a real isolated rsyslog TCP receiver, temporary directories and dummy
Compose values. They verify restart/append, recreated sender connections, UTC date
templates, two-day retention, size/oversized-record trimming, safe scope, redaction
and all merged service settings. The
midnight fixture substitutes reported time into the same date template instead of
changing host time; production uses receipt time.

The initial PAPER VPS installation, receiver append/restart, cleanup timer and host
boot recovery passed; CURRENT_STATE.md owns that evidence. Remaining acceptance
includes sender recreation, a real UTC day boundary/expiry, production trimming,
receiver/timer failures and sustained disk growth while trading state stays intact.
Local fixture checks alone do not establish those results.

References: [Compose override requirements](https://docs.docker.com/reference/compose-file/merge/),
[Docker syslog](https://docs.docker.com/engine/logging/drivers/syslog/),
[Docker cache/delivery limits](https://docs.docker.com/engine/logging/dual-logging/) and
[rsyslog templates](https://docs.rsyslog.com/doc/configuration/templates.html).
[Rsyslog program output](https://docs.rsyslog.com/doc/configuration/modules/omprog.html)
documents the receiver/writer integration.
