# Step 14A — Verified RealProvider Foundation

Version: **v0.14.0**

This step is intentionally a foundation gate, not a cosmetic release and not yet a live-data release.

## What changed

- Reconciled the dashboard with the trading project after T24.
- Recorded verified PostgreSQL table ownership and checkpoint state keys.
- Recorded verified post-T23 NATS trading subjects (no legacy clock subjects).
- Added Go read-only DTOs matching the verified C++ JSON/persistence shapes.
- Added explicit per-resource source/projection status for `RealProvider`.
- Kept `real` resource reads fail-closed until concrete adapters are tested.
- Bumped dashboard API version to `0.14.0`.

## What did NOT change

- No C++ trading code.
- No database migrations.
- No writes to trading PostgreSQL.
- No real NATS consumer.
- No frontend page contract changes.
- No Manual Control execution.
- No exchange connectivity.
- Mock mode remains the current runnable UI mode.

## Acceptance gate

From `dashboard/dashboard-api`:

```text
gofmt: clean
go test ./...: PASS
go vet ./...: PASS
go build ./...: PASS
```

In addition:

- `RealProvider` remains not-ready/fail-closed.
- all 12 resources have an explicit real-data mapping status.
- verified runtime NATS subject list contains no clock subject.
- verified PostgreSQL table/state-key tests pass.
