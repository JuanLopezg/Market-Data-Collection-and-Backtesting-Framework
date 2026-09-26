# Step 18 — Real Execution

Step 18 enables `/api/execution` in `DASHBOARD_DATA_PROVIDER=real`.

Canonical sources:
- current/tracked order state: `trading_runtime_state.snapshot.orders`
- durable fills and commissions: `trading_fills`

Safety/semantic rules:
- read-only PostgreSQL only; no execution command path is added
- newest 200 tracked orders max; 1000 fill rows max for displayed order IDs
- latency is explicitly unavailable because the durable snapshot does not persist gateway/venue wall-clock latency
- slippage is explicitly unavailable because no canonical expected-price reference is persisted with tracked orders
- cycle/correlation/replacement lineage are not invented
- reject reasons use persisted `last_message` when available

The existing Execution UI is real-aware: it labels unavailable quality fields with `—` / `Not available` rather than displaying fake zeroes.
