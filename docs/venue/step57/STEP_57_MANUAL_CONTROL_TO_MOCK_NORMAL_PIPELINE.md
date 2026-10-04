# Step57 — Manual Control -> MOCK normal pipeline

Bound Step56A acceptance fingerprint: `0ec48c4566c7a1ba439164c2f242f061b8adf58eb801a536f34b8c12d55f64ae`.

Step57 closes the **trading-control core path** for a confirmed manual target without enabling any real venue.

The canonical flow is:

`confirmed/hash-bound manual target -> ManualPortfolioRiskEngineV1 -> DecisionBatch -> NotionalOrderPlannerEngine -> CanonicalVenueAdapter -> MockExchangeAdapterV1 -> Step50 admission -> Step51 Fill -> Step52 accounting -> Step54/55 reconciliation gate`

## Manual target contract

A manual target contains request/correlation identity, actor, request hash, close(T) decision time, later open(T+1) execution time, explicit long-only asset weights, and explicit cash weight. Asset + cash weights must sum to 100% within a frozen tolerance.

No symbol guessing is allowed. Every non-cash asset must have an explicit enabled Step49 MOCK catalog/rules leg.

## Risk semantics

`ManualPortfolioRiskEngineV1` is the missing PortfolioRisk-side manual transformation. It applies the shared `RiskConstraints` primitive. Step57 deliberately **rejects** a request when hard constraints would mutate the operator's requested target rather than silently routing a different target.

The engine emits the same `DecisionBatch` contract consumed by the production `NotionalOrderPlannerEngine`.

## Planning and execution

The production notional planner is used. Quantities are created only at the venue edge, using close(T) for notional reference and open(T+1) for limit reference, with explicit MOCK grids. FLAT uses the exact fill-derived strategy quantity to avoid rounding residue.

All submit/cancel commands cross `CanonicalVenueAdapter`. There is no direct risk/planner -> MOCK submit bypass.

## Safety

Before any new manual exposure, Step54/55 reconciliation must be CLEAN and fresh. Cancel is sent before a replacement submit. Canonical Step51 Fill is the only authority that changes the manual strategy-position mirror. Step52 remains accounting authority.

## Dashboard boundary

Step57 intentionally does **not** turn the existing Step46 dashboard HTTP route into a process-local exchange call. `dashboardHttpTransportEnabled=false` is frozen. Step58 owns the dashboard simulation transport/read-model wiring so the browser remains separated from the trading runtime.

The included CLI is a trading-control harness for the Step57 C++ pipeline, not a browser endpoint.

No Hyperliquid private auth/signing, real routing, smart routing, or live capital is enabled.

Next: **Step58 — Dashboard Simulation Completion**.
