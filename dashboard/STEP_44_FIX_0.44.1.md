# Step 44 Fix 0.44.1 — bounded SQLite busy/locked handling

Observed blocker: the canonical market-data SQLite reader can return `database is locked (5)` while the runtime/dashboard/watchdog are concurrently accessing the canonical database. The endpoint correctly stayed fail-closed, but the Step 35 regression gate only classified `SQLite query timed out` as transient.

Fix:
- keep SQLite strictly `-readonly` and `query_only=ON`;
- add connection-local `PRAGMA busy_timeout=1000` so a short writer lock can clear without an immediate false negative;
- extend the already-bounded Step 35 retry classifier to the exact canonical-strategy errors `database is locked` / `database is busy`;
- extend Step 44 retryability classification for the same bounded read-only condition;
- semantic mapping errors, missing classifications, divergences, malformed responses, auth failures and any other blockers still fail immediately.

No wallet, signing, order submission/cancellation or trading mutation was added.
