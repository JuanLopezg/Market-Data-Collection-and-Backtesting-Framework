# Indicators, selection, ranking and market-filter behaviour

Purpose: turn completed observations into causal features and an eligible, ordered
entry universe. Wrong smoothing, warm-up, volume units or tie order can change which
positions receive capital. These components produce features/eligibility, not orders
or fills.

Sources, in order: `docs/codex/CONTEXT_FULL.txt` (PAPER quote-volume selection),
`docs/codex/ARCHITECTURE_FULL.md` (completed close/open timing and turnover),
`lib/README.md` (missing turnover and SMA recovery), public indicator, universe,
ranker and filter headers, and `config/strategies/pure_rsi_quote_volume.json`.
Numerical examples below are hand calculations, not observed program output.
Where the contract does not settle a convention, it is marked (assumed).

## Rules and worked examples

R1. Every calculator returns one slot per supplied observation. Insufficient history
is NaN, never a usable zero. A window of length N needs N observations for SMA,
EMA and extrema, or N+1 for N changes/true ranges (RSI, ROC, ATR). Example: three
closes 10,20,30 give two unavailable slots then SMA(3)=20, but no RSI(3) yet.
Empty input gives an empty vector. Length zero gives only unavailable slots
(assumed invalid-configuration policy; no financial feature may be usable).

R2. SMA is the equally weighted average of the latest N observed source values,
including the completed current bar. Example: closes 10,20,30,40,50 and N=3 give
NaN,NaN,20,30,40. Source selection is exact: bars
`{O=10,H=13,L=8,C=12,V=5,Q=41}` and `{O=20,H=25,L=17,C=22,V=7,Q=63}` give
two-bar means O=15,H=19,L=12.5,C=17,V=6,Q=52. Actual quote turnover is never
estimated from volume*close. A window containing missing/nonfinite values is
unavailable; once they leave it, SMA recovers. Example: quote turnover
10,NaN,30,50,70 and N=2 give NaN,NaN,NaN,40,60. Negative quote turnover is
invalid, while zero is a real observation: 0,20 has mean 10.

R3. EMA uses an N-bar simple-average seed, then alpha=2/(N+1) (assumed standard
seed convention). Example: closes 10,20,30,50,10 and N=3 give
NaN,NaN,20,35,22.5: 0.5*50+0.5*20=35, then 0.5*10+0.5*35=22.5.
N=1 reproduces each current source value, without look-ahead.

R4. RSI uses Wilder averages of gains/losses: seed from N price changes, then
average_new=((N-1)*average_old+current)/N. RSI=100*gain/(gain+loss).
Example: closes 10,12,11,13,12,14 with N=3 have initial gains 4/3 and losses
1/3, hence RSI at index 3 is 80. At index 4 the averages are 8/9 and 5/9,
giving 800/13 (about 61.53846). At index 5 they are 34/27 and 10/27, giving
850/11 (about 77.27273). All gains -> 100; all losses -> 0; no movement -> 50
(assumed neutral flat-series convention). Valid RSI is in [0,100]. Multiplying
all prices by a positive constant or adding a fixed constant does not change it.

R5. ROC is a fractional return, value(T)/value(T-N)-1, not percentage points.
Example: 100,110,90,120 with N=2 gives NaN,NaN,-0.1,1/11. A zero denominator
is unavailable, not infinity: base-volume sequence 0,5,0 at N=1 gives
NaN,NaN,-1. Only the configured source and lag contribute.

R6. ATR follows the public calculator header's **simple moving average** of true
range, starting at index N using TR[1]..TR[N]. TR=max(high-low,
abs(high-previous_close),abs(low-previous_close)); the first bar has no previous
close. Example bars (O,H,L,C) are (100,102,98,100), (102,103,99,102),
(104,108,102,106), (110,116,108,112). TR[1..3] are 4,6,10. ATR(2) is
NaN,NaN,5,8, where the final value is (6+10)/2=8. This requirement is from the
header; a recursive Wilder result of 7 would disagree with it. Gaps must count:
the last true range is 116-106=10, larger than its intrabar range 8.

R7. Highest/Lowest use the full latest N observations including T (assumed current
bar convention); expired extremes leave the window. Example: 10,30,20,5 at N=2
give Highest NaN,30,30,20 and Lowest NaN,10,20,5. Donchian bands apply these
windows to High and Low, with midpoint=(upper+lower)/2. Highs 12,15,14,20 and
lows 8,9,7,11 at N=2 give uppers NaN,15,15,20, lowers NaN,8,7,7 and midpoints
NaN,11.5,11,13.5. Prior-bar bands are obtained explicitly with offset=1.

R8. Calculations are causal even when the engine precomputes full future history.
Adding or changing data after T cannot change any feature at or before T. Example:
SMA(2) of 10,20,30 at its last bar is 25 even if the next close is 1000000.
Offsets refer to earlier **observations of that asset**, not later bars or calendar
days. Example: closes 10 on Jan 1, 20 on Jan 3, 30 on Jan 4 give SMA(2)=15
on Jan 3 and 25 on Jan 4; offset=1 on Jan 4 returns 15. The first offset slot
and offsets beyond available history are NaN. No missing calendar bar is fabricated.

R9. The indicator cache distinguishes kind, source, length and offset. Missing
asset, date, specification or warm-up data returns NaN with `has=false`; a valid
computed zero is available. Example: BTC closes 10,20,30 give SMA(2)=25 and
SMA(3)=20 at Jan 3, while constant ETH=5 gives its own SMA=5. An unrequested
EMA cannot reuse SMA's cache entry. Repeated/duplicate-spec precompute is
economically idempotent. Recompute replaces old assets/specs/timestamps and owns
its computed values independently of later caller mutation. Rebuilding a new engine
from the same prefix reproduces results; no durable checkpoint exists here.
For a cache-key worked example, closes 10,20,30,50,10 with volume=1 on each
observation give, at the fourth bar: SMA(Close,3)=100/3, SMA(Close,2)=40,
EMA(Close,3)=35, SMA(Volume,3)=1 and SMA(Close,3,offset=1)=20.

R10. Invalid arithmetic must not advertise a usable financial feature (assumed
defensive policy for corrupt raw input). `has` must be false for a nonfinite result,
not merely for NaN. Example: Highest(High,1) from high=+infinity is not available
for a strategy/risk decision. Selectors and rankers with skipInvalid=true must
independently exclude nonfinite scores. Invalid data cannot win a liquidity contest
because infinity sorts above finite turnover.

R11. Liquidity selection is confined to current candidate bars. It does not search
all cached assets or select by today's turnover when a moving average was requested.
Example: two-day quote volumes A=(100,300), B=(300,500), C=(200,400),
OUTSIDE=(9000,11000) give means 200,400,300,10000. Candidates {A,B,C}, N=2,
descending=true -> {B,C}; OUTSIDE is ineligible despite its history. Ascending
selects {A,C}. Current base volumes A=1000,B=1,C=20 do not affect quote-volume
selection; explicitly selecting SMA(Volume,2) instead gives {A,C}. Selected bars
retain their current prices/volumes; this layer never resizes quantities or fabricates
observations. Zero top-N/empty candidates yield empty; an oversized top-N retains
all eligible candidates once.

R12. Missing liquidity is skipped by default; it is never estimated. Example:
candidates A with quote SMA=200 and D with missing quote history yield {A}, even
when N=2. With skipInvalid=false, unavailable scores may be retained **after**
finite ones (assumed ordering convention). Finite equal scores and mutually
unavailable scores use alphabetical symbol order to make boundary membership
deterministic (assumed tie policy). Example: A and B both mean 200 with N=1 give
{A} regardless of candidate insertion order. All-missing {C,B,A} with N=1 and
skipInvalid=false also give {A}. Re-delivery/reconstruction or a future turnover
spike cannot change the selection at T.

R13. Indicator ranking orders only current eligible candidates by the configured
feature/direction and assigns consecutive 1-based ranks. Example: one-day ROC
A:100->110 =0.1, B:100->80 =-0.2, C:100->100 =0. Descending order is
A,C,B with ranks 1,2,3; ascending is B,C,A. Ranked bars remain the same current
financial observations (e.g. B's close remains 80), and reported scores retain
fractional units. Equal scores use alphabetical order (assumed deterministic tie
policy). Default missing/invalid scores are excluded, but valid zero is included.
With skipInvalid=false, finite scores precede unavailable ones and unavailable ties
are alphabetical (assumed). No symbol outside the candidate set can gain a rank.

R14. Alphabetical ranking gives deterministic symbol order without a feature
dependency. Example: unordered candidates C,A,B produce A,B,C and 1-based ranks
1,2,3, consistent with the public RankedCoin contract. Different hash insertion
orders or a fresh ranker cannot alter the result. Current bar prices remain intact.

R15. Benchmark-above-SMA is a strict entry gate: current benchmark close > its
configured-source SMA at the same T. Equality fails. Example: BTC closes 10,20,30
give SMA(3)=20 and pass on the third bar; 30,20,10 give 20 and fail;
20,20,20 give 20 and fail. Missing date, missing benchmark, insufficient/uncomputed
SMA or invalid/nonpositive SMA fail closed. Nonfinite current benchmark close must
also fail closed (assumed corrupt-input policy). The source config controls the
average, while comparison stays against close: opens 40,40,40 with closes 10,20,30
give open-SMA=40 and fail. A later bar cannot change an earlier gate result.

R16. Real collaborators must compose using their declared indicator requirements.
Quote liquidity determines eligibility before signal ranking; benchmark filtering
decides whether entries are permitted, not which liquidity units to use. Worked
example: three closes per asset are A=100,120,100; B=100,110,100; C=100,90,110.
With RSI(2), A=50, B=50, C=200/3. Two-day quote turnover means are A=100,
B=400,C=300, so liquidity top-2 gives {B,C}; RSI descending then gives C(rank 1),
B(rank 2). OUTSIDE may have arbitrarily high liquidity and RSI but is not a
candidate. BTC benchmark closes 10,20,30 pass their SMA(3) gate. Precompute the
union of selector/ranker/filter requiredIndicators(), then use completed T for all
three stages. Repeating this evaluation leaves input observations unchanged.

## Timing, invariants and error handling

- Inputs are synthetic, deterministic and keyed by explicit YYYYMMDD dates. History
  means observed bars, not elapsed calendar days. No wall clock, sleeps or network.
- At close(T), features and gate decisions use only completed observations through T;
  offset=1 means an earlier bar. Open(T+1) and execution fills are outside this area.
- Missing/invalid features cannot silently become numeric zero or an eligible score.
  Zero itself can be valid. Base volume and actual quote turnover remain distinct.
- Pure selection/ranking/filtering does not mutate source bars or retain hidden
  per-day signal state. Restart equivalence means rebuilding engines from the same
  history and rerunning the same pure decisions, not restoring trading positions.
- Length-zero handling is covered as unavailable. Finite positive valid OHLC input
  is normal; corrupt-input assumptions above are explicit rejection/availability
  requirements for later triage, not claims about valid feed output.

## Findings

Not evaluated. This specification and its tests precede the first indicators build
or run. Assumptions and the public ATR definition remain explicit for later triage.
