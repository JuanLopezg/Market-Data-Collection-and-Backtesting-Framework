# Step 15 — Real read-only source adapter foundation

Version: 0.15.0

This step does **not** switch dashboard screens to real data yet. It adds the safe connection boundary needed before doing that.

## Added

- Bounded TCP reachability probes for PostgreSQL and NATS.
- Read-only SQLite file/header validation for the canonical market-data database.
- `GET /api/source-status` (authenticated).
- Real-provider configuration now carries actual DSN/URL/path values instead of presence booleans.
- `docker-compose.real.yml` opt-in overlay that joins the existing `algotrading-live_live` network and mounts `../storage/databases` read-only.
- Unit tests for DSN parsing, NATS URL parsing, TCP probes and SQLite validation.

## Still deliberately blocked

- No PostgreSQL SQL queries from dashboard-api yet.
- No NATS subscriptions or JetStream consumers yet.
- No dashboard page has switched from fixtures to live state.
- No trading-control commands exist.
- `/api/readiness` remains not-ready in real mode because resource reads are still fail-closed.

## Why

We first prove that the dashboard can reach the three verified source families without mutating runtime state. Resource integration starts in Step 16, one read model at a time.
