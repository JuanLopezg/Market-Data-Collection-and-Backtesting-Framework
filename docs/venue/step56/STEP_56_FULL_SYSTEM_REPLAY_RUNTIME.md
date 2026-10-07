# Step56 — Full-System Replay Runtime Wiring

**Delivery prerequisite:** Step54 and Step55 are treated as assumed PASS for this
delivery because the user requested the next step without running the Step55 gate.
The Step56 gate re-runs Step55 first, so one local command validates the dependency.

**Bound Step55 fingerprint:** `7bb82df68ea1187bd59bb40c9907a0186c8829935da623a6538d2809ac6f16ba`

This step wires the production engine boundaries into the MOCK venue stack:

`closed market slice -> StrategySignalEngine -> PortfolioRiskEngine ->
NotionalOrderPlanner -> CanonicalVenueAdapter -> MockExchangeAdapterV1 ->
admission -> matching/fills -> accounting -> recovery -> reconciliation/ledger ->
chaos/rate limit`

## Important distinction

Step56 **builds and validates the full-system replay runtime path**.

The large historical dataset replay campaign itself is intentionally the next action
after this gate. `actualHistoricalDatasetCampaignExecuted=false` is frozen in the
manifest so this wiring test cannot be confused with the acceptance run.

## Canonical venue boundary

Step56 introduces the first complete concrete implementation of the Step48 interface:

`MockExchangeAdapterV1 : CanonicalVenueAdapter`

Strategy/Risk/Planner never receive a concrete MOCK runtime reference. Trading commands
cross the canonical interface.

The concrete adapter separately exposes MOCK-only replay-control methods for:
- historical bar injection;
- explicit mark-to-market;
- reconciliation;
- chaos controls.

Those are simulation-control surfaces, not trading-command bypasses.

## Event phases / no lookahead

For each historical timestamp:

1. orders admitted at execution open `T+1` already exist in MOCK;
2. when the `T+1` bar closes, MOCK matching consumes that completed bar;
3. canonical Fill events update Step52 accounting and the Step56 strategy-position
   compatibility mirror;
4. held positions are marked at close;
5. only then is the closed slice appended to `RollingMarketState`;
6. Strategy sees close `T+1`;
7. Risk sees canonical equity + fill-derived strategy positions;
8. Planner creates the next USD-notional plan;
9. that plan cannot be submitted until a later execution-open event.

Therefore Strategy never sees a future closed slice when creating the previous order.

Business timestamps are the historical release timestamps supplied by the upstream
TimeHandler/replay controller. Step56 contains no wall-clock economic source.

## Planner -> venue edge

The planner remains exchange-agnostic.

At the MOCK edge:
- quantity starts from `notional_usd / close(T)`;
- quantity floors to the explicit MOCK size increment;
- buy limit is `ceil(open(T+1), price tick)`;
- sell limit is `floor(open(T+1), price tick)`;
- Step50 remains the authority for minimum-size/min-notional/TIF/identity admission.

No symbol heuristic exists: canonical asset -> explicit Step49 MOCK catalog entry.

## Batch and reconciliation

At each execution open:
- planner cancels are sent first as one canonical cancel batch;
- if new orders exist, Step54/55 reconciliation must be CLEAN/fresh;
- all new planner orders are sent in one canonical submit batch.

Batching matters because a successful submit advances venue sequence and therefore
makes reconciliation stale for the *next* exposure decision, not for sibling items in
the same atomic planner batch.

## Fill authority and planning mirror

Canonical Step51 `Fill` remains the only source of execution truth.

Step56 maintains a compatibility `OrderManager` and strategy-position mirror only so
the existing `NotionalOrderPlanner` can calculate current/pending quantities.

That mirror:
- changes strategy positions only from canonical Fill;
- does not own cash/PnL/equity;
- does not replace Step52 accounting;
- maps canonical lifecycle events into the legacy planner OrderManager.

## PortfolioRisk compatibility bridge

The existing PortfolioRisk `AccountSnapshot` recomputes equity as:

`cash + physical positions * close`

That is spot-style accounting, while Step52 is a linear perpetual account.

Until that older contract is deliberately migrated, Step56 passes:
- `cash = canonical Step52 equity`;
- `positions = empty`;
- `strategy_positions = fill-derived quantities`.

This prevents perpetual notional from being double-counted as spot inventory.
The bridge is internal only. User-facing/canonical accounting remains Step52.

## Concrete adapter query surfaces

The MOCK canonical adapter also exposes:
- account snapshot;
- open orders;
- order status;
- fill backfill;
- canonical event handler;
- deterministic `poll()` release for delayed responses.

## Run evidence

Step56 carries fingerprints for Steps49–56 and produces:
- economic fingerprint;
- user-stream fingerprint;
- ledger head hash;
- chaos evidence fingerprint;
- full-run SHA-256 fingerprint;
- counts for closed slices, opens, signal/risk/planning batches, fills and accounting.

## Local gate scope

The local gate uses a small deterministic threshold strategy and synthetic bars to prove
the *wiring* through the real StrategySignalEngine, PortfolioRiskEngine and
NotionalOrderPlanner.

It deliberately does not claim the real PureRSI historical dataset campaign has run.

After this gate passes, the next action is the actual **Full-System Replay Acceptance
Campaign** using the historical source/replay controller and the configured strategy.
