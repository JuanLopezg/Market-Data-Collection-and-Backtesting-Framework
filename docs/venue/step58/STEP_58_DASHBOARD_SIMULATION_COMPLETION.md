# Step58 — Dashboard Simulation Completion

Bound Step56A acceptance fingerprint: `0ec48c4566c7a1ba439164c2f242f061b8adf58eb801a536f34b8c12d55f64ae`  
Bound Step57 manual-control fingerprint: `65c0a7b418d7f3873f38f6a1daa87d5d915e4a254452865b79a69df24c3dc5f0`  
Step58 implementation fingerprint: `ed65f18d0720f95c60443319d5e559407fa50340d9dc67f46c19868d20ae3852`

## Purpose

Run the accepted historical PureRSI full-system pipeline and the operational dashboard at the same time against one canonical MOCK truth domain.

The browser remains dashboard-only. It never receives NATS credentials, PostgreSQL credentials, venue credentials, a private key, or a direct venue transport.

## Runtime path

Historical OHLCV -> TimeHandler -> PureRSI -> PortfolioRisk -> production OrderPlanner -> CanonicalVenueAdapter -> MockExchangeAdapter -> Step50 admission/lifecycle -> Step51 Fill -> Step52 accounting -> Step53 recovery/user stream -> Step54 reconciliation/ledger -> Step55 chaos/rate limit -> Step58 atomic simulation snapshot -> dashboard-api -> browser.

The dashboard simulation provider reads only `state.json`. SSE is invalidation-only: generation changes tell the browser to refetch normal authenticated REST read models; SSE does not carry trading data.

## Visible canonical evidence

Step58 supplies existing dashboard pages with current MOCK evidence for market data, orders/fills, positions, canonical cash/equity/unrealized PnL, Step54 ledger, CLEAN/BLOCKED/PENDING reconciliation, infrastructure/simulation phase, canonical reconciliation alerts and human manual-control outcomes.

Where evidence is not canonical yet, the dashboard says so. In particular, Live vs Expected remains `INSUFFICIENT_DATA`; Step58 does not invent a statistical baseline. Step59 owns deterministic baselines/speed invariance/restart evidence.

## No-lookahead display

At `REPLAY_OPEN`, the state snapshot contains only the open price for the current bar: high=low=close=open and volume=0. The completed OHLCV bar is published only at `REPLAY_CLOSE`, after MOCK matching for already-admitted orders. This prevents the dashboard bridge from exposing a future close as if it were known at the open.

UI pacing (`STEP58_UI_DELAY_MS`, default 350 ms per phase) is technical visualization pacing only. Economic/business timestamps are historical TimeHandler/event timestamps and do not depend on wall clock.

## Manual Control

After the historical window finishes, the runner enters `MANUAL_READY` only when the account handoff is flat, reconciliation is CLEAN and Step55 reports routing safe.

The simulation provider can then accept an OPERATOR-confirmed CSV. dashboard-api recomputes the server preview/hash and writes an fsynced atomic request file in the shared simulation directory. The runner consumes that file and invokes the already-validated Step57 manual Risk -> production Planner -> CanonicalVenueAdapter -> MOCK path. Browser -> venue remains impossible.

The REAL provider does not implement this simulation routing interface and therefore keeps the Step46 fail-closed behavior.

## Visual acceptance required

The automated gate proves compilation, read-model conversion, SSE invalidation contract, short replay state publication and manual file-transport -> Step57 execution. Step58 is only fully closed after the user also runs the dashboard + default replay and visually confirms that the relevant pages move with the replay without fabricated values.

## Next after visual acceptance

Step59 — Deterministic Evidence / Speed Invariance / Restart.
