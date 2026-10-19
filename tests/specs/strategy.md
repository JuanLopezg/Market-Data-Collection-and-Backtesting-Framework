# Strategy and signal behaviour specification

Purpose: generate persistent exposure opinions from completed observations, without
confusing a signal with a position, weight, order, quantity or fill. This area covers
PureRSI, SignalState, StrategySignalInstance and StrategySignalEngine. Sizing,
rebalance and fill-owned StrategyInstance behaviour belong to the risk/execution
areas rather than being reimplemented here.

Sources: `docs/codex/CONTEXT_FULL.txt`, `docs/codex/ARCHITECTURE_FULL.md`,
`lib/README.md`, public strategy/signal headers and `strategy_intent_batch.h`.
All examples below use synthetic completed daily closes. Unspecified requirements
are marked (assumed); expected results are hand-derived, not copied from a run.

## Rules and worked examples

R1. SignalState stores finite opinions in [-1,+1], including fractional and short
signals for general strategies. Zero means absent, not an active allocation slot.
Example: A=1,B=-0.25,C=0.5 has three active opinions; closing A leaves only B and C,
unchanged. Repeating a signal or closing A twice does not duplicate activity.
NaN, infinity, 1.01 and -1.01 throw out_of_range without replacing an accepted
opinion or adding a new invalid one. Restoring a complete signal map replaces old
signals and canonicalizes zeros. Invalid restore is atomic at the instance level:
restoring {B=0.5,C=2} must preserve the previous {A=1}.

R2. PureRSI creates only full long +1 signals, strictly when RSI(length)>entry.
No available RSI means no new signal; a threshold is not a crossing requirement.
For length 2, closes 10,13,12 give gains 3 and losses 1 across two changes,
RSI=100*3/(3+1)=75. At entry=75 that does not enter; 10,14,13 give
RSI=100*4/5=80 and do enter; 10,12,11 give 200/3 and do not enter.
Only two prices cannot provide two changes, so length-2 RSI cannot enter yet.
If a previously excluded asset already has RSI=80 when it becomes eligible, it
can enter without a fresh threshold crossing.
For a length-7/entry-80/exit-70 configuration, closes
100,101,102,103,104,105,106,105,102 have no RSI at bar 7, RSI=600/7 at bar 8
(six gains of 1 and one loss of 1), and Wilder RSI=400/7 at bar 9 (gain average
36/49, loss average 27/49). They enter at bar 8 and exit at bar 9.

R3. A positive active signal exits strictly when its own finite RSI<exit. Equality
retains it. Example: with entry=75, exit=50, closes 10,11,10 give RSI=50 and
retain A=1; 10,11,8 give RSI=25 and exit; 10,14,13 give 80 and retain it.
For exit=25, closes 10,11,7 give RSI=20 and exit. An absent target-date price or
insufficient RSI must not be interpreted as an exit. A direct strategy update on a
missing whole timestamp leaves existing signals unchanged.

R4. Signals persist across bars until their own exit condition fires, independently
of whether a fill has occurred. Worked lifecycle, length 2/entry 75/exit 25:
closes 10,14,13,12,7,16 give RSI at bars 3..6 of 80,400/7,400/27,7600/99.
The corresponding signal is +1,+1,0,+1. Bar 4's RSI is below entry but above exit,
so it must not liquidate or disappear. Repeating a direct strategy evaluation of an
unchanged completed T yields the same opinions; stream-level duplicate rejection
is owned separately by StrategySignalEngine.

R5. Exits run before entry filters, liquidity selection, ranking and capacity checks.
Example: cap=1, held A has RSI=20 and B has RSI=80; at the same close, A exits
and B can occupy the newly freed slot. A failing benchmark filter can block B but
cannot suppress A's exit. An active symbol outside today's entry candidate bars
still exits from its available raw indicator history. The market timestamp must
exist, but the held symbol need not be in its entry cross-section.

R6. Universe selection controls only new signals. Quote liquidity is an entry
eligibility gate, not a reason to flatten old signals. Example: A and B both have
RSI=80, quote-volume SMA(2) A=100 and B=400, liquidity top-1 -> only B can newly
enter. If A is already active and capacity=2, it stays active and B may also enter.
An asset present in raw history but absent from current candidate bars cannot newly
enter. Unknown quote turnover is never replaced with base-volume*close to qualify.

R7. New entries follow the ranker's order and stop at the configured maximum
ranking position, counted from 1 across **all** ranked candidates. Example:
alphabetical A(RSI=75),B(RSI=80), entry=75, maxRankingPosition=1 -> no entry;
B is second even though A fails entry. RSI-descending ranking puts B first and
allows B under that same limit. An already active rank-1 A still occupies rank
position 1; skipping it does not promote B into the first position. Rank limit zero
blocks new entries but does not block exits.

R8. maxActiveSignals bounds the number of active opinions, not a sum of monetary
weights. Example: cap=1 and two qualifying candidates A,B -> one enters according
to ranking, and repeating evaluation leaves one signal. An existing valid signal
uses a slot and is not evicted for a stronger newcomer. Cap=0 creates no new
signals but still permits an already active signal to exit. An existing restored
state above the configured cap is retained until actual exits, rather than having
arbitrary positions implicitly dropped (assumed persistence policy).

R9. All configured market filters must pass for new entries. Filters are evaluated
at the same close and cannot force a still-valid existing opinion to zero. Example:
benchmark closes 10,20,30 have SMA(3)=20 and pass; 30,20,10 have SMA(3)=20
and fail. With two such benchmark gates, the second failure blocks a qualifying
A even if the first passes. A missing benchmark or its warm-up blocks entry.
Null filter slots are ignored, consistent with shared strategy setup.

R10. The required-indicator declarations must make real collaborators work without
hand-populating scores. Example: PureRSI(2), quote SMA(2) top-1, RSI(2) ranking and
benchmark SMA(3) use the union of their requirements. A=10,14,13 with quote
turnover 100 on each bar and B=20,28,26 with turnover 400 on each bar both have
RSI=80; benchmark 10,20,30 passes. Only B enters the quote top-1 universe. Its
signal is +1, independent of its close 26, base volume or any account capital.
Two eligible independent longs can both have +1: their signals are not normalized
to portfolio weights 0.5/0.5.

R11. StrategySignalEngine advances only through explicit completed timestamps.
It rejects timestamp zero, duplicate/older timestamps and a timestamp absent from
the current market data, without advancing its frontier or changing opinions.
Example: after processing Jan 3, Jan 3 again and Jan 2 are rejected; Jan 4 is
accepted only when its completed cross-section exists. RollingMarketState supplies
the released history in integration tests. Merely having a future raw bar or an
execution-open price is not a closed-slice release. The API's onBarClose input is
already a completion boundary; detecting an unlabelled intraday candle is the
caller's responsibility, not an invented flag in this API.

R12. Each successful engine evaluation returns a complete signal snapshot for
every configured strategy, including empty/unchanged signals, with configured
strategy ID, name and the requested close timestamp. Example: two PureRSI
instances, ID 7 entry=75 and ID 9 entry=85, see A's RSI=80: ID 7 emits {A=1},
ID 9 emits {}. The next neutral day still includes ID 7's A=1, rather than an empty
delta. Previously returned snapshots are independent values: a later exit cannot
rewrite the earlier {A=1}. State is isolated per strategy; opinions never aggregate
across IDs and do not imply fills or available equity.

R13. A signal instance must own a non-null strategy. Engine portfolios must be
nonempty and IDs unique, or construction fails. Example: two instances with ID 7
are rejected, while IDs 7 and 9 remain independent. Restore must have nonzero
timestamp, exactly the configured strategy set, no duplicate IDs, matching names
and valid finite opinions in [-1,+1]. Example: configured {7,9} rejects checkpoint
{7,8}, duplicate {7,7}, a missing record, a renamed strategy or a signal 2.
No rejected checkpoint may advance the last processed date.

R14. Restart restores the exact durable signal snapshot and frontier without
re-evaluating entry conditions at the checkpoint date. Indicator history is supplied
separately at the next completed close. Example: checkpoint at bar 3 of R4 contains
{A=1}; a fresh engine restores it and bar 4 retains +1, bar 5 exits, just as the
uninterrupted engine does. A restored checkpoint's same timestamp cannot be
processed again. Restore matches records by ID, not vector position, and replaces
absent old opinions rather than merging them. Repeating the identical restore is
idempotent. Failed restore must leave **all** configured strategies' signals and
the frontier unchanged, even if the error is in a later strategy record (assumed
transactional restore policy). Example: existing ID 7 {A=1}, ID 9 {B=1}; proposed
ID 7 {C=1} plus invalid ID 9 name/signal must leave {A=1}/{B=1}, not half-apply C.

R15. Close-T intent must be causal. Inserting future bars into precomputed history
or changing opens, prices of unrelated assets, or base volume cannot change an
already determined pure close-RSI opinion at T. Example: 10,14,13 at Jan 3 gives
RSI=80 and +1; adding Jan 4 close 1 cannot suppress that Jan 3 entry. Its eventual
execution at open(T+1) is a later risk/planning/execution responsibility. When only
Jan 3 is released in rolling history, onBarClose(Jan 4) fails until that slice is
actually appended. Reconstruction from the same accepted prefix and checkpoint
must preserve the subsequent opinion path.

## Invariants and boundaries

- Synthetic deterministic observations only; explicit YYYYMMDD timestamps; no clock
  reads, sleeps, network, real databases or installed datasets.
- PureRSI-generated opinions are +1 or absent. SignalState's general short/fractional
  vocabulary is validated separately; the tests do not invent short PureRSI entries.
- No fill is fabricated by entry, hold, exit, replay or restore. There is no equity,
  fee, execution quantity or accounting input to the signal-only engine.
- Direct strategy evaluation is a persistent-state transition. The engine enforces
  stream timestamp ordering; durable service publish/checkpoint/ACK and transport
  duplicate IDs are outside this in-process area.
- An unavailable indicator retains an old opinion and cannot create a new one. Raw
  history for old holdings can exceed today's entry universe, without creating entries
  for excluded assets. Future changes cannot alter a completed-T opinion.

## Findings

Not evaluated: specification and tests precede the first strategy build or run.
Assumed restore/persistence policies remain explicit requirements for later triage.
