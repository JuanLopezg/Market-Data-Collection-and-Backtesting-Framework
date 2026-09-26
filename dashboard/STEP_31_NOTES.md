# Step 31 — End-to-End Proof

Step 31 adds a **read-only, fail-closed proof of the latest durable trading cycle** to the existing Pipeline page.

It does not generate a signal, publish NATS, submit an order, request a fresh exchange snapshot, or alter PostgreSQL/SQLite state.

## Evidence chain

The proof correlates only canonical evidence already used by the dashboard:

1. Market Data — exact cycle-aligned `strategy_market_update_checkpoint.update_payload`.
2. Strategy — exact persisted `StrategyIntentBatch` from the PortfolioRisk checkpoint.
3. Risk / Approved Target — exact persisted `DecisionBatch`.
4. Order Planning — exact same-cycle OrderPlanner request + plan.
5. Execution Order — planned order IDs matched against `trading_runtime_state.snapshot.orders`.
6. Persisted Fill — planned order IDs matched against `trading_fills`; FILLED quantities must agree with durable ExecutionState quantities.
7. Reconciliation — current local state compared with fresh retained `execution.exchange.snapshot.v1` evidence.
8. Dashboard Projection — confirms the evidence was assembled through the read-only real provider.

## Verdict semantics

- `ALIGNED / FULL_CHAIN`: a cycle with submit orders has complete, internally consistent durable evidence through fill and reconciliation.
- `PENDING / NO_ACTION`: the latest cycle legitimately contains no submit order, so Step 31 refuses to pretend there is an order/fill chain to prove.
- `PENDING / INCOMPLETE_OR_IN_FLIGHT`: order/fill/reconciliation evidence is not complete yet.
- `BLOCKED / CONTRADICTION_OR_TERMINAL_FAILURE`: rejected/canceled submit flow, reconciliation block, or a durable FILLED-vs-fill quantity contradiction prevents a full proof.

The latest cycle may therefore show `PENDING` on a quiet system. That is expected and is safer than manufacturing a test trade.

## Local WSL verification

Start the normal local stack exactly as before, then open `/pipeline`. The new **Step 31 · End-to-End Proof** panel appears above the Decision Matrix.

A command-line read-only check is also included:

```bash
./scripts/step31-e2e-proof.sh
```

To make a CI/manual gate require a complete aligned execution chain:

```bash
DASHBOARD_PROOF_REQUIRE_ALIGNED=1 ./scripts/step31-e2e-proof.sh
```

Do not use the strict form on a quiet/no-action live cycle unless a real trading action is expected to have completed.
