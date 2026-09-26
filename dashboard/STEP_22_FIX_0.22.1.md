# Step 22 fix v0.22.1 — SQLite WAL/read-only mount compatibility

Observed in the real stack:

- `/api/source-status` could read the SQLite header.
- `/api/market-data` failed with SQLite error 14 (`unable to open database file`).

Cause: the canonical live SQLite directory can require writable WAL/shared-memory
sidecar bookkeeping when SQLite opens the database, even when the connection is
strictly read-only. Mounting the whole directory `:ro` blocked that open path.

Fix:

- mount the market-data directory writable so SQLite can access/create sidecar
  coordination files;
- over-mount `/data/market/database.db` itself read-only;
- keep the query connection `sqlite3 -readonly`;
- additionally force `PRAGMA query_only=ON` and `PRAGMA temp_store=MEMORY`.

The dashboard does not issue INSERT/UPDATE/DELETE/DDL statements.
