# Step 44 fix 0.44.2 — SQLite JSON stdout isolation

## Failure observed

The canonical market-data SQLite reader returned:

`decode SQLite JSON result: invalid character '[' after top-level value`

The 0.44.1 contention fix configured the CLI with `PRAGMA busy_timeout=1000` via `-cmd` while also using `sqlite3 -json`. SQLite's `busy_timeout` PRAGMA returns its current numeric value, so stdout could contain the timeout value followed by the JSON query result. The Go reader correctly rejected that as more than one top-level JSON value.

## Fix

- Keep the database opened with `-readonly`.
- Keep `PRAGMA query_only=ON`.
- Keep in-memory temp storage.
- Replace output-producing `PRAGMA busy_timeout=1000` with the sqlite shell `.timeout 1000` command.
- `.timeout` configures the bounded busy wait without adding a result row to JSON stdout.
- Add a regression test that forbids `PRAGMA busy_timeout` in the CLI argument list and requires `.timeout 1000`.

## Safety

No malformed/decode error is converted into PASS or retryable state. The JSON decoder remains strict. Symbol mapping, registry, ledger, watchdog, private-auth and routing semantics are unchanged.
