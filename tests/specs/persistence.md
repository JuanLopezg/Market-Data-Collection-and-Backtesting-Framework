# Persistence and startup recovery behaviour

## Purpose and sources

Persist the operational truth required to resume trading without losing cash, changing
order intent or applying a fill twice. Reconciliation compares that truth with an
exchange snapshot and permits trading only when they agree. SQLite and PostgreSQL
implement the same `StateStore` contract; PostgreSQL tests use an isolated Docker
database, never an external database.

Sources: `docs/codex/ARCHITECTURE_FULL.md` (Durability and replay timing),
`lib/README.md` (execution and recovery boundaries), `state_store.h`,
`trading_state_snapshot.h`, `execution_order.h`, `fill.h`, `reconciler.h`,
`exchange_snapshot.h`, `recovery_coordinator.h`, and `postgres_state_store.h`.
Rules explicitly labelled `(assumed)` specify necessary behaviour where those public
contracts do not prescribe it. Container readiness is technical time; all economic
timestamps in the tests are explicit daily `YYYYMMDD` keys.

## Storage rules

**R1 — Absence differs from a saved flat account.** A fresh store returns no snapshot
and no fills. A saved snapshot with no positions is durable, even with zero cash.
Example: fresh -> absent; save cash 0, next order ID 1 -> present with cash 0 and
zero positions after reopening. Reading must not create an operational snapshot.

**R2 — Snapshots round-trip without economic loss.** Preserve schema, both progress
timestamps, next order ID, signed holdings, per-strategy signals/weights/virtual
holdings, pending plan timestamps/reference capital/actions, order lifecycle and
partial quantities, cancellation intent, external IDs/messages, and processed fill
IDs. Compare keyed collections by identity, not vector/hash iteration order. An
omitted strategy with no signals, weights or holdings is semantically empty (SQLite
schema-v1 compatibility). Example: cash 849, BTC +2, ETH -0.5; strategy 7 owns BTC +2
and strategy 9 owns ETH -0.5. A BTC buy of 5 with 2 filled has 3 remaining; the pending
plan at 20260102 retains capital 1000 and BTC weight 0.25 (= USD 250), ETH Flat,
and absent SOL means Hold. At a later open price 125, USD 250 means 2 units, not
250/close(T). Storage does not execute this plan. Also preserve representable double
precision: cash 1234.5678901234567 and quantity 0.12345678901234566 must not be rounded
to six decimal places on restart.

**R3 — Save replaces operational state, never the fill audit.** Rows removed from the
next snapshot must not reappear on restart. Example: snapshot A has 2 BTC, a pending
plan, order 41 and processed ID 101. Saving flat snapshot B with cash 1048, no orders,
plans, strategies or processed IDs removes all A's operational children; the
previously committed fill 101 remains readable. Clearing operational processed IDs
here tests replacement only, not a valid runtime deduplication policy.

**R4 — Stable fill IDs make audit writes idempotent.** An identical `(snapshot, fill)`
redelivered twice yields one audit record and the same operational values. A later
checkpoint without a new fill keeps that record. Distinct IDs are distinct partial
fills even if their other fields match. Example: start cash 1000; buy 2 BTC at 75,
fee 1 -> cash 849, BTC 2, processed {101}. Save twice -> still cash 849 and one
fill of 2, not BTC 4. A second buy 1 at 75, fee 0.5 -> cash 773.5, BTC 3, two fills.
The caller supplies already-applied economics; store deduplication does not apply or
undo account cash movements. Fill list iteration order is unspecified; identity and
all fields must survive. Fill ID 4294967297 is distinct from ID 1 (no 32-bit truncation).

**R5 — Snapshot and new audit fill commit atomically.** Validation or database failure
leaves both the previous snapshot and its audit unchanged. Example: saved cash 1000,
no fills; attempt cash 849 + fill 101, but fail the snapshot database write after
the audit insert -> cash remains 1000 and fill count remains 0. Remove the injected
failure and retry -> cash 849, one fill. Failure injection uses a temporary database
trigger, not a mock store. Invalid fills (ID 0, empty symbol, nonpositive/nonfinite
quantity or price, negative/nonfinite fee) are rejected. Example: quantity 0 spends
no valid amount and must never replace cash 1000 with 849. Errors are exceptions;
the same store must remain usable after rollback.

**R6 — Unknown durable schemas fail closed (assumed for PostgreSQL).** A schema the
reader does not understand must not be returned as safely recovered state. Example:
persist schema 2 when CURRENT_SCHEMA_VERSION is 1 -> load throws. This specifies
reader safety, not a requirement that save reject an unknown version.

**R7 — Schema v1 refuses unsupported research conditional intent.** Entry triggers,
research protective triggers and bracket parent linkage must not be silently dropped.
Reject before changing the previous snapshot/audit. Example: BTC weight 0.25 with
entry trigger 110 is not a market buy; saving it alongside fill 101 must throw and
leave cash 1000/no fills. The same applies to an order with entry trigger 110,
protective trigger 120, or parent order ID 40. This is distinct from LIVE position
stops, represented by their own public order field.

**R8 — Preserve venue-relevant order intent (assumed for SQLite).** A staged order's
reference price, immediate reference-price fill flag and LIVE position-stop price
must survive restart, because recovery may later publish it. Example: manual BTC
buy 2 at reference price 75 with immediate-fill flag true must recover those values;
a reduce-only-purpose BTC sell 2 with position_stop_price 60 must recover 60, not
become an unconditional sell. No exchange request or fill occurs during save/load.

## Reconciliation rules

**R9 — Compare cash alone with an inclusive absolute tolerance.** With default
tolerance 0.000001, local cash 0 vs exchange 0.000001 is clean; vs 0.000002 gives one
CashMismatch with both values. Use a binary-exact custom tolerance 0.125 for boundary
checks: 1000 vs 1000.125 clean, 1000 vs 1000.25 mismatch. Exchange BTC collateral USD
500 is not cash and must not cause drift when both cash balances are 1000. Neither
input is repaired/mutated by comparison.

**R10 — Compare the union of signed positions; absence means zero.** Local BTC +2 vs
exchange +2 is clean; exchange -2 is a PositionMismatch (difference 4). Local-only
BTC +2 and exchange-only ETH -0.5 produce two issues, including the asset and both
quantities. An explicitly zero holding vs absence is clean. Strategy allocations
and pending targets are intent and are not exchange holdings.

**R11 — Quantity tolerances suppress rounding, not real drift.** Accept differences
<= max(configured absolute tolerance, 1e-12 * max(abs(local), abs(exchange))).
Example with absolute 0.125: 2 vs 2.125 clean, 2 vs 2.25 mismatch. With defaults,
1,000,000 units vs 1,000,000.0000005 is clean (difference 0.0000005, threshold about
0.000001); vs 1,000,000.001 is drift. Apply the same rule to order total and filled
quantities. Equal remaining quantity is insufficient: 5 total/2 filled vs 6/3
both leave 3, but represent different fills and must mismatch.

**R12 — Match live orders by stable identity, once each.** Match local order ID first,
or nonempty exchange order ID when local mapping is unavailable. Compare asset,
side, original quantity and filled quantity. Example: accepted local order 41,
external ID `venue-41`, BTC Buy 5, filled 2 matches exchange local ID 41 (even with
a different display/external ID), or local ID 0 plus external ID `venue-41`.
Neither ordering the input vectors differently nor comparing twice changes the
report. Two local accepted orders claiming one exchange identity cannot both match;
one remains missing. A symbol, side, total or filled discrepancy gives OrderMismatch.

**R13 — Distinguish unsent intent from missing or unexpected exposure.** A Created
order absent at exchange is safe staged intent; Submitted/Accepted/PartiallyFilled
orders absent there are MissingExchangeOrder. A cancel-requested order remains live
until terminal confirmation. Filled/Canceled/Rejected local orders require no
exchange open order. Example: accepted BTC Buy 5, filled 2 missing at exchange ->
issue local remaining 3/exchange 0. Unknown exchange ETH Sell 4, filled 1 ->
UnexpectedExchangeOrder with local 0/exchange remaining 3. An exchange order for a
locally terminal order is also unexpected. Reconciliation reports differences only;
it never cancels, resubmits or credits cash.

**R14 — Invalid numeric comparisons must not be clean.** Reject negative or
nonfinite configured tolerances. Nonfinite cash, positions or order quantities are
blocking differences, even if both sides contain the same infinity. Example:
local cash NaN vs exchange cash 1000 -> CashMismatch; BTC +infinity vs +infinity ->
PositionMismatch. Zero tolerances are valid: 2 vs 2 matches, 2 vs 2.0000001 does not
(unless within the fixed relative allowance).

## Startup recovery rules

**R15 — Pause, restore, reconcile, then resume/checkpoint only if clean.** Recovery
uses persisted truth rather than overwriting it with the new process's initial
account. It restores IDs, progress, holdings, pending intent and partial order state
without submitting/canceling orders. Example: fresh engine cash 1000; persisted
cash 849/BTC 2/order 41 partly filled 2 of 5/next ID 42 agrees with exchange ->
restored=true, ready=true, trading enabled, unchanged 3 remaining. If exchange cash
is 848, restore still occurs but ready=false/trading disabled, old database remains
intact, and strategy/execution entrypoints reject new work. With no persisted
snapshot, matching fresh cash 1000 may start and writes its first checkpoint; an
exchange-only BTC 2 blocks and leaves the database absent. Repeating clean recovery
must not add fills or duplicate trade analytics.
An incompatible configured strategy set must also fail closed (assumed): persisted
strategy ID 99 with only IDs 7 and 9 configured throws, stays paused, and does not
replace the saved snapshot.

**R16 — Audit rebuild is analytics, never a second economic application.** Rebuild
closed campaigns from committed fills, ordered by timestamp then fill ID, without
reapplying them to account state. Example: buy 2 at 75 fee 1 (101, 20260102), sell 2
at 100 fee 1 (102, 20260103) -> gross PnL 50, fees 2, net closed PnL 48, final cash
1048/BTC 0. Even when inserted in reverse chronology and recovered twice, there is
one closed trade, PnL 48 and cash 1048. Persisted operational dedup IDs prevent a
redelivered fill 102 from changing that result. Fills without an operational snapshot
are an error and trading remains paused; they are not enough to reconstruct signals
or pending plans. Test that inconsistent state via isolated SQLite deletion.
The same deduplication applies to an open campaign: recovering the buy alone, then
redelivering its fill ID 1, keeps cash 849/BTC 2, one recorded fill, fee 1, and marked
PnL -1 at price 75 (= 2*75 - 2*75 - 1).

**R17 — PostgreSQL durability extends beyond the client object.** A committed snapshot
and fill survive a database-container restart, not only reconstruction of the C++
store. Example: save cash 849/BTC 2/fill 101, restart PostgreSQL, reconnect -> the
same snapshot and one fill. Independent readers after a committed replacement see
the new snapshot; an uncommitted/failed replacement exposes neither half (R5).

## Invariants and boundaries

- Cash, positions, fees and quantity signs are copied, never recomputed by storage.
- No save/load/reconcile/recover operation consumes future market bars or fills a plan.
  A close(20260102) pending plan stays pending for open(20260103), preserving the
  original reference capital and decision timestamp.
- Processed fill IDs and the append-only audit have separate responsibilities; both
  survive normal restart. Historical daily keys are never replaced by wall time.
- Real SQLite/lib engine collaborators are used in process. Only the external venue
  is represented by `FakeExchange`. PostgreSQL cases start with the prescribed Docker
  availability skip and run in a separate `service` executable with timeout 300.
- Accounting projections and PostgreSQL venue-cost/equity APIs are outside the shared
  StateStore contract covered here; execution-state/accounting areas own those rules.

## Findings

### F1 — SQLite drops manual execution intent (R8)

`SQLiteBackend.DISABLED_R8_ManualReferencePriceAndImmediateFillIntentSurvive` retains
the original expected snapshot. The first run failed because a staged manual buy of
2 BTC saved with `reference_price=75` and `fill_at_reference_price=true` reopened
with `reference_price=0` and `fill_at_reference_price=false`. The SQLite tracked-order
schema, INSERT and SELECT in `lib/src/persistence/sqlite_state_store.cpp` omit both
fields. This loses the specified sizing/reference-price execution intent on restart.
The public `ExecutionOrder` comments assign economic meaning to these fields, so this
is a code/spec disagreement, not a change to the expected result.

### F2 — SQLite drops the LIVE position-stop trigger (R8)

`SQLiteBackend.DISABLED_R8_LivePositionStopRemainsConditionalAfterRestart` retains the
original expected snapshot. The first run failed because a staged BTC sell of 2 with
`position_stop_price=60` reopened with `position_stop_price=0`. SQLite save accepts
the snapshot but its tracked-order schema and codec omit the field. By the public
`ExecutionOrder::isPositionStop()` contract, this turns a position stop into an
unconditional order after restart. The spec remains unchanged.

The two R8 cases were moved from the shared typed suite into backend-specific tests
so only the observed SQLite failures carry `DISABLED_` and `// FINDING:` comments.
Their PostgreSQL equivalents remain enabled. No expected value was changed.

### F3 — PostgreSQL loads a snapshot of an unknown schema version (R6)

Found when the Docker-backed executable first ran against a real PostgreSQL 16
(orchestrator, 2026-10-19; it was skipped during step 3). A snapshot saved with
`schema_version = CURRENT_SCHEMA_VERSION + 1` is returned by
`PostgresStateStore::load` after reopening, while `SQLiteStateStore::load` refuses
it as R6 requires. Both implement the same `StateStore` contract, and PostgreSQL is
the store the VPS chain uses, so an older binary reading newer durable state would
not fail closed at this layer (the release `STATE_FORMAT` check in `deploy/chain/algo`
is a separate, coarser guard). R6 is now checked per backend:
`SQLiteBackend.R6_LoadRejectsUnknownSchema` passes and
`PostgresBackend.DISABLED_R6_LoadRejectsUnknownSchema` keeps the expectation.

### Step 3 validation

- `ninja -C build -j2` compiled both persistence executables without warnings.
- Initial in-process run: 43 tests, 41 passed, 2 failures (F1 and F2); all 20
  reconciliation and 8 recovery tests passed.
- PostgreSQL executable: all 17 tests skipped with `Docker is not available`.
  PostgreSQL behaviours, including the assumed unknown-schema rejection, have not
  been validated against a running server and are not claimed as findings.
- After triage, rebuilt with `ninja -C build -j2` and ran
  `meson test -C build --no-rebuild --print-errorlogs unit_persistence unit_persistence_postgres`:
  60 registered cases = 41 passed, 2 disabled findings, 17 Docker skips, 0 remaining
  failures. Meson reports both executables OK; the PostgreSQL result represents
  skips, not server-backed passes. Detailed final output is in
  `build/meson-logs/testlog.txt`.

### Fixed (2026-10-19, orchestrator)

- F1/F2: `SQLiteStateStore` persists `reference_price`, `fill_at_reference_price` and
  `position_stop_price`; older files gain the columns with neutral defaults.
- F3: `PostgresStateStore::load` refuses a snapshot whose schema version is not current.

The corresponding tests are enabled again with their original expectations.
