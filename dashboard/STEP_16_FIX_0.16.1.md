# Step 16 fix v0.16.1

Fixes the real PostgreSQL runtime summary query.

PostgreSQL does not provide `jsonb_object_length(jsonb)`. The dashboard now counts
`account_positions` with `jsonb_object_keys(...)` and guards all JSON collection
counts with `jsonb_typeof(...)`, so absent or JSON-null fields return zero instead
of failing the whole read-only endpoint.
