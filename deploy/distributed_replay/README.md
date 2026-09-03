# Distributed replay compose

This is the first complete live-like replay topology. The historical file is mounted only
into `market-data`; every downstream component receives released contracts through NATS.

## Build the runtime image inputs

From the project root:

```bash
ninja -C build -j8
bash deploy/distributed_replay/build_runtime_bundle.sh
```

The bundle is generated under `deploy/distributed_replay/.runtime_bundle/` and contains
only the eight service executables, `libalgolib.so`, runtime shared libraries and config.
It is intentionally generated and should not be committed.

## Run

```bash
REPLAY_START_DATE=2024-01-01 \
REPLAY_END_DATE=2024-01-03 \
docker compose -f deploy/distributed_replay/docker-compose.yml up --build
```

The replay controller exits with code 0 when the requested range completes. Long-lived
services remain running so their state/logs can be inspected. Stop with:

```bash
docker compose -f deploy/distributed_replay/docker-compose.yml down
```

Use `down -v` only when you explicitly want to delete PostgreSQL and JetStream replay state.
