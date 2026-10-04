# Step47A Future Venue Portability Checklist

Contract: `step47a-v1`

This checklist is intentionally venue-neutral. `FUTURE_VENUE` is a placeholder, not an
implemented exchange.

A future venue is architecturally admissible only if all answers remain YES:

- [x] Can it receive canonical economic/order intent without changing Strategy?
- [x] Can Risk remain venue-neutral and rely only on canonical rules/capabilities?
- [x] Can OrderPlanner remain free of native protocol payloads and native reject strings?
- [x] Can asset routing use an explicit per-venue Symbol Registry leg with no heuristic mapping?
- [x] Can native order/fill/client IDs be retained as evidence without changing canonical ID types?
- [x] Can native errors map to canonical error classes while retaining the raw reason/code?
- [x] Can authentication/signing stay fully inside the concrete adapter/edge?
- [x] Can the venue declare unsupported capabilities and fail closed rather than pretend support?
- [x] Can account/open-order/fill truth reconcile within an explicit venueId/environment domain?
- [x] Can the same dashboard pages/read models display the venue as data instead of branching into a parallel UI?
- [x] Can MOCK, the first real venue, and FUTURE_VENUE coexist behind one future Step48 contract?
- [x] Can explicit configuration choose the venue without silent fallback or implicit smart routing?

Out of scope for v1:
- smart order routing between venues;
- split execution across venues;
- automatic failover from one real venue to another;
- automatic REAL -> MOCK fallback;
- cross-venue netting/reconciliation semantics.
