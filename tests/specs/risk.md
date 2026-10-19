# Risk, sizing, portfolio targets and rebalancing

Purpose: convert exposure opinions into approved monetary targets while preserving
held quantities whenever the chosen policy says HOLD. Sizing, hard limits,
rebalance decisions and capital conversion are separate economic steps; none is a
fill. Sources: `docs/codex/CONTEXT_FULL.txt`, `docs/codex/ARCHITECTURE_FULL.md`,
`lib/README.md`, public risk/sizing/portfolio/rebalance headers and account/decision
contracts. Additional requirements are explicitly marked (assumed). All examples
are hand calculations using synthetic completed daily observations.

## Rules and worked examples

R1. EqualWeightSizer multiplies each signal by its configured full-signal weight;
it does not divide by the number of active assets. Example: full weight 0.25 and
signals A=1,B=0.5,C=-0.25 give weights 0.25,0.125,-0.0625. Adding another
signal must not shrink those existing desired weights. Empty signals or configured
weight zero give an available empty target, not unavailable sizing.

R2. Sample covariance uses the latest N valid aligned close-to-close **simple**
returns, sample denominator N-1, annualized by explicit periodsPerYear. Example:
A closes 100,125,93.75 give +0.25,-0.25; B closes 100,50,75 give -0.5,+0.5.
Both means are zero. With N=2 and periods/year=2, covariance is
[[0.25,-0.5],[-0.5,1]]: A variance (0.0625+0.0625)/(2-1)*2=0.25.
These values are dimensionless return covariances, not USD variances.

R3. Covariance at T cannot use a later date and must use common observations for
every requested asset, never pairwise samples or a bridge across a missing market
timestamp. Example: removing B's middle bar in R2 invalidates both adjacent return
observations, so there is no two-return estimate. Invalid/nonpositive closes also
make their return observation unavailable. With older A prices 50,100,125,93.75,
N=2 uses only the last +0.25,-0.25, not the older +1. Adding a future price 1000000
cannot change the R2 covariance at the third close. Fewer than N valid returns
give nullopt. Duplicate asset requests and invalid estimator configuration throw.

R4. Portfolio volatility is sqrt(w^T covariance w), keeping signed weights and
cross-covariances. Example: covariance [[0.25,0.5],[0.5,1]] with weights
A=1,B=-0.5 has variance 0.25-0.5+0.25=0 and volatility zero; changing B to
+0.5 gives variance 1 and volatility 1. An unknown covariance asset is an error;
materially negative portfolio variance must throw instead of becoming zero. An
empty portfolio has volatility zero. With identical A/B return histories from R2's
A series and weights 1,0.5, the raw volatility is 0.75.

R5. Volatility targeting scales the original signal shape by target/estimated
annualized volatility, preserving its signs and proportions. Example: raw shape
A=1,B=-0.5, estimated volatility 0.5, target 0.25 -> scale 0.5 -> weights
0.5,-0.25. No arbitrary equal-weight seed or account capital is introduced.
Real SampleCovarianceEstimator on R2's A series has volatility 0.5, so target
0.25 gives A weight 0.5. Identical A/B histories with shape 1,0.5 and target
0.375 give scale 0.5 and weights 0.5,0.25. Multiplying annualization from 2 to
8 multiplies volatility by 2 and halves weights at the same target.

R6. Unavailable covariance or zero estimated volatility with active signals and a
positive target means sizing is deferred (nullopt), never an instruction to flatten
or invent leverage. Example: only two closes cannot supply two returns; flat
closes 100,100,100 give volatility zero, so a positive target cannot be sized.
Empty signals remain an available empty target even without history and permit
actual exits. Target volatility zero disables exposure: VolatilityTarget returns
empty weights, and a zero-target sizer should do so without requiring history
(assumed zero-target disable policy for VolatilityTargetSizer).

R7. Hard constraints cap individual absolute weights **first**, then proportionally
scale down gross=sum(abs(weight)) if necessary. Example: desired A=0.8,B=-0.6,
C=0.2, asset cap 0.5 -> 0.5,-0.5,0.2, gross 1.2; gross cap 0.6 -> scale
0.5 -> 0.25,-0.25,0.1. Signs survive. No constraint increases exposure; applying
the same constraints again is idempotent. Either limit zero disables exposure;
zero weights are absent. Optional cap observations must report these actual
intermediate/final amounts without changing the result. Invalid finite/range
configuration must throw; weights and monetary targets must never store infinity.

R8. Capital enters only when approved dimensionless weights become monetary
exposures. Example: weights A=0.25,B=-0.125 with USD 1000 capital give
USD +250,-125. Zero capital gives no exposure; negative/nonfinite capital or an
overflowing product is rejected. Portfolio aggregation nets independent monetary
targets by asset without rescaling or another capital multiplication: portfolios
{A=250,B=-125} and {A=-250,C=50} give {B=-125,C=50}. Input order cannot
change this result and source portfolios remain unchanged. Identical independent
strategy portfolios legitimately add; this helper has no message-ID deduplication.

R9. EntryExitOnly policy sizes a new active position, flattens an inactive held
position, reverses a changed side, and otherwise HOLDs the actual filled quantity.
Example: signal +1,target 0.25,quantity 0 -> TARGET_WEIGHT(0.25); same signal
and held quantity 2 -> HOLD even if desired weight later becomes 0.5. Signal zero
with quantity +2 or -3 -> FLAT; signal -1,target -0.25,held +2 -> TARGET(-0.25).
An approved zero weight flattens a nonzero holding even if its signal is active.
HOLD contains no target notional; it cannot be interpreted as a resize to the
currently evaluated desired weight.

R10. Absolute-threshold policy compares abs(desired-currentWeight) to the configured
absolute weight threshold, inclusive, where currentWeight=quantity*close/capital.
Example: quantity 1,close 250,capital 1000 give current 0.25. Threshold 0.125:
target 0.3125 differs by 0.0625 -> HOLD; target 0.375 differs by 0.125 ->
TARGET. Entry, exit and side reversal bypass drift thresholds. Valuing a held
same-side position requires finite positive price and capital. Nonfinite signal,
weight or quantity and invalid threshold configuration are rejected.

R11. Relative threshold compares drift to threshold*abs(currentWeight), inclusive.
Example: threshold 0.5,current weight 0.25 gives the HOLD band (0.125,0.375);
targets 0.125 and 0.375 resize, while 0.3125 HOLDs. With current 0.125 the band
narrows to (0.0625,0.1875). The public 20% example also applies literally:
current 10%,target 12%,threshold 20% resizes at equality (economic decimal
boundary equality is assumed to tolerate floating representation error). Entry,
exit and reversals bypass thresholds just as with the absolute policy.

R12. Engine strategy capital is allocation_weight times account equity at the same
completed T. Equity=cash+haircut-adjusted collateral_usd+sum(real signed quantity
*close(T)). Virtual strategy quantities are not counted a second time. Example:
cash 1000,collateral 200,real A=+2 at 100,real B=-1 at 50 give equity 1350.
Allocation 0.4 gives capital 540; equal weight 0.25 with A=1,B=-0.5 signals gives
USD targets +135,-67.5. Negative cash is permissible when total equity is positive:
cash -100 plus A=2 at 100 gives equity 100 and a 0.25 target is USD 25.
Independent strategies with allocations 0.6 and 0.4 of equity 1000 and weights
+0.25 and -0.5 receive USD +150 and -200, not an implicitly netted signal.

R13. The engine applies sizing, caps and policy before emitting a monetary intent.
For R7 signals 1,-0.75,0.25 at equal weight 0.8, equity 1000 and allocation 0.5,
approved weights 0.25,-0.25,0.1 become USD +125,-125,+50. Every explicit
TARGET/FLAT carries a matching target notional; FLAT is zero. All-HOLD strategies
are omitted, preserving quantities. Example: held A=2,close=100,cash=800 gives
equity 1000; sized weight 0.8 capped at 0.1 is observable, but entry/exit policy
emits no resize and A remains 2. Missing signal on a held asset still generates
FLAT, for longs and shorts alike. Unavailable sizing emits no new intent and leaves
holdings untouched; available empty sizing does generate exits. Target production
never mutates the account projection or pretends a fill occurred.

R14. Signals and account must refer to exactly the same nonzero completed T.
Duplicate/older signals, mismatched account date, missing T market data, wrong
strategy IDs/names, duplicate/missing intents, invalid opinions, missing configured
strategy positions, nonfinite position quantities, missing/invalid held-account
closes, invalid collateral or nonpositive equity fail rather than produce unsafe
targets. Example: signal Jan 3 plus account Jan 2 or Jan 4 is rejected. A rejected
cycle does not advance lastTimestamp and may be retried with corrected inputs.
Restart restores a nonzero frontier without moving it backwards; after restoring
Jan 3, reprocessing Jan 3 fails and a valid Jan 4 cycle can continue. An explicit
restore(0) reset is a separate supported API operation, not an implicit retry.

R15. Target-decision helpers use explicit long-only target weights and reference
capital; held omitted assets are flattened if a price exists, while missing-price
assets are HOLD by omission. Example: capital 1000,weights {A=0.25},held B=2,
prices A=100,B=50 -> TARGET A USD250 and FLAT B USD0. If B's price is absent,
there is no B decision. Manual resync can add formerly all-HOLD strategies from
available approved evaluations and resize them to approved targets. However every
existing **non-HOLD** decision must be kept, as stated by the public helper header:
an existing explicit A FLAT must not become a new A TARGET merely because an
evaluation has A=0.25. Unavailable sizing preserves the original decisions and does
not create new strategies. All transforms preserve their timestamp/capital lineage.

R16. Confirmed manual weights must be accepted exactly or rejected, not silently
clipped by risk. Example: BTCUSDT weight 0.8 plus cash 0.2,asset limit 0.5 is
rejected with no executable decisions. BTCUSDT 0.25 plus cash 0.75 at equity
1000 with unit caps is approved at USD250; omitted held ETHUSDT=3 becomes FLAT
at zero notional. Only known enabled MOCK assets with exact closing references
qualify. Cash weight is not an executable asset. These are local synthetic intents,
not permission to submit orders.

R17. StrategyInstance rebalance plans obey the same boundaries as the standalone
risk engine. Example: restored A signal +1,held quantity 2,capital 1000,close 100,
equal size 0.8,asset cap 0.1,entry/exit-only policy -> HOLD,quantity remains 2;
the evaluated desired weight may be 0.1 but no trade has occurred. If an active
volatility sizer cannot estimate covariance, calculateRebalancePlan returns nullopt
and retains both existing quantities and previously known desired weights.

R18. Optional observations must not change trading decisions. Example: A returns
+0.25,-0.25 at annualization 2 give raw volatility 0.5. Target 0.25 scales A to
0.5; asset cap 0.25 reduces it to 0.25. At capital 1000, an entry targets USD250.
Diagnostics report raw=0.5,scale=0.5,pre-cap vol=0.25,post-cap vol=0.125 and
the approved weight 0.25, with the same USD intent whether observations were
requested or not. Future bars cannot change these T quantities or equity marks.

## Timing, invariants, duplicate and restart handling

- Trace: completed market history -> signals -> covariance/sizing -> caps -> policy
  -> approved weights -> capital -> USD targets -> later planning/execution/fills.
  Only the first part is exercised here; no new positions, cash or PnL are invented.
- At close(T), risk cannot read future closes or future account state. Quantity
  conversion at open(T+1) remains a planner/execution responsibility.
- Signed quantities and weights are preserved through equity, covariance, caps and
  USD conversion. Gross uses absolute exposure and never offsets a long with a short.
- HOLD preserves filled quantity, even when capped desired weights or volatility
  diagnostics differ from that holding. Manual resync is an explicit separate action.
- Test data is synthetic, deterministic and dated by explicit YYYYMMDD keys; no
  Internet, clocks, sleeps, external datasets, PostgreSQL or NATS are required.
- Pure sizing/constraints/aggregation may be recomputed without mutation. The risk
  engine rejects duplicate dates and restores its frontier; transport IDs and actual
  durable service checkpoints belong to service-level tests.

## Findings

The 60 tests were written from this specification before the first risk build/run.
The first build passed without compiler warnings in the new tests. The first direct
run exited 1 with 57 passes and three code-versus-spec disagreements. All three
retain their original assertions under `DISABLED_` names with FINDING comments.
No test mistakes or specification corrections were identified; expected values and
production code remain unchanged. Explicit assumptions retain their policy status.

F1 (R6, zero-target sizing still requires covariance). Active signal A=1, target
volatility 0 and empty market history produce nullopt, rather than an available
empty target. Inspection shows VolatilityTargetSizer requests covariance and
returns early when it is unavailable, before applying VolatilityTarget's zero-target
rule. The separate VolatilityTarget zero-target test and no-active-signals sizer
test both pass. This disagrees with the explicitly assumed zero-target disable
policy: deferral preserves old holdings, whereas an available empty target lets
the rebalance policy remove exposure. Retained test:
`RiskSizing.DISABLED_R6_ZeroVolatilityTargetDisablesExposureWithoutRequiringHistory`.

Fixed (2026-10-19): Return an available empty target for zero target volatility before requesting covariance.

F2 (R11, relative threshold misses its decimal equality boundary). Quantity 1,
close 100,capital 1000 imply current weight 0.10. Desired weight 0.12 differs by
0.02, exactly 20% of 0.10. The policy returns HOLD with target_weight=0 rather
than TARGET_WEIGHT(0.12). Inspection confirms direct floating subtraction and
comparison without a rounding allowance: the represented drift is slightly below
the represented threshold product. Binary-exact boundary tests pass; this finding
is specifically the published 10% -> 12% example and the spec's assumed economic
decimal equality tolerance. Retained test:
`RebalancePolicies.DISABLED_R11_PublishedTwentyPercentRuleResizesAtTwelvePercentEquality`.

Fixed (2026-10-19): Allow a small scale-dependent floating-point tolerance at the relative rebalance boundary.

F3 (R15, manual resync rewrites an explicit exit). The input decision is A FLAT
with USD0; the available evaluation has A weight 0.25 and B weight 0.5 at capital
1000, with held A=1,B=2 and both prices available. Resync correctly adds B's
USD500 target but replaces A's explicit FLAT with TARGET_WEIGHT(0.25)/USD250.
The public `target_decisions.h` contract says every existing non-HOLD decision is
kept. Inspection shows the implementation rebuilds all decisions from approved
weights, including those already explicit, instead of only moving prior HOLDs.
The original input remains unchanged; the returned decision is the disagreement.
The test deliberately supplies differing explicit decision/evaluation intent to
exercise that preservation promise; it does not claim normal engine evaluations
always differ from their decisions. Retained test:
`TargetDecisions.DISABLED_R15_ManualResyncPreservesAnExplicitExitWhileResizingOtherHolds`.

Initial evidence: `ninja -C build -j2`, then
`ALGOTRADING_SOURCE_ROOT=$PWD build/tests/unit/risk/test_unit_risk
--gtest_output=xml:build/tests/unit/risk/first_run.xml`. To reproduce a retained
finding after triage, add `--gtest_also_run_disabled_tests` and select its full
disabled name with `--gtest_filter`.

Final verification after triage: `ninja -C build -j2` passed without test-code
warnings. Direct execution with `ALGOTRADING_SOURCE_ROOT=$PWD
build/tests/unit/risk/test_unit_risk
--gtest_output=xml:build/tests/unit/risk/triaged.xml` exited 0: 60 registered
tests, 57 passed, 3 disabled, zero enabled failures. No production code or test
assertions/expected values were changed during triage.
