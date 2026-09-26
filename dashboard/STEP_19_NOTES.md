# Step 19 — Real Reconciliation

Step 19 enables the Reconciliation page in real-provider mode.

## Sources

- PostgreSQL `trading_runtime_state.snapshot` for durable local cash, positions, and tracked orders.
- JetStream `ALGOTRADING_RUNTIME`, latest retained `execution.exchange.snapshot.v1`, obtained through the JetStream read API.
- C++ source audit: `Reconciler::compare` and `ExchangeSnapshot` contract.

## Safety

The dashboard performs no exchange snapshot request and no trading publication. It uses only JetStream management/direct-get reads. Missing or stale exchange evidence produces `PENDING`. Fresh mismatches reproduce the runtime reconciliation issue classes and produce `BLOCKED`.

## Still unavailable

Risk-approved target quantity/value is not inferred. That belongs to the planner/risk lineage and will be wired in a later step.
