# Step 14 local Docker fix — v0.14.1

This package is based on the user's current dashboard folder.

Fixes:
- `dashboard-web` now joins both `dashboard-public` and `dashboard-private` in local Docker Compose.
- Port `8080:80` can therefore be published to the host while the API remains private.
- Removed stale generated metadata (`tsconfig.app.tsbuildinfo`, `desktop.ini`).
- Confirmed obsolete duplicate source files are absent (`real_views.go`, `verified_schema.go`).

Local run:

```bash
docker compose down --remove-orphans
docker compose up -d --build --force-recreate
docker compose ps
```

Expected web port:

```text
0.0.0.0:8080->80/tcp
```

Then open `http://localhost:8080`.

Consistency cleanup:
- Removed partial Step 14B PostgreSQL/libpq smoke code that was not wired into the Step 14A provider and caused `go test ./...` failures.
- Consolidated NATS subject verification onto `subjects.go` (removed duplicate declarations).
- Updated PostgreSQL contract tests to validate the audited Step 14A schema constants that actually exist.
