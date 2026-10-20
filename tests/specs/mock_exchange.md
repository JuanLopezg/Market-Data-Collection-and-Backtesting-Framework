# Canonical MOCK exchange behaviour specification

## Purpose and sources

MOCK is the deterministic canonical venue: exact admission -> accepted order ->
synthetic matching -> fills/accounting -> durable source replay -> reconciliation.
No fill may exist without an admitted order, aggregate fills cannot exceed its
quantity, and recovery must preserve the same economics and identities.

Sources, in priority order: `docs/codex/{CONTEXT_FULL.txt,ARCHITECTURE_FULL.md}`,
`lib/README.md` (MOCK and replay boundaries), frozen specifications in
`docs/venue/step50/STEP_50_MOCK_ORDER_ADMISSION_AND_LIFECYCLE.md`,
`step51/STEP_51_DETERMINISTIC_MATCHING_FILL_MODEL.md`,
`step52/STEP_52_MOCK_ACCOUNT_MARGIN_POSITIONS_ACCOUNTING.md`,
`step53/STEP_53_MOCK_SNAPSHOT_USER_STREAM_RECOVERY.md`,
`step54/STEP_54_RECONCILIATION_LEDGER_PARITY.md`,
`step55/STEP_55_FAULT_CHAOS_RATE_LIMIT_ENGINE.md`, and current public headers in
`lib/src/exchange/mock/` plus `contracts/venue_{orders,account,events,errors}.h`.
Frozen guide class names may predate the current source names; contracts remain V1.

Synthetic fixtures use explicit MOCK/BTCUSDT/Perpetual/USD catalog identity,
strategy 7, initial settled cash USD 100,000 and 400 ppm (0.04%) fee unless stated.
Business event keys 10 (submit), 20 (eligible bar), 30 (later bar) are supplied
explicitly; they are not wall-clock time. Default orders BUY 0.2 at limit 100,
active from 20. Normal tests disable adverse slippage to isolate arithmetic while
retaining the real participation limit. Tests of slippage use a bounded invariant.
Real `MockOrders`, `MockMatching`, `MockAccount`, `MockRecovery`, `MockReconciliation`,
`MockFaults` and `MockExchange` collaborate; durable tests use fresh temporary local
directories. No storage data, network, services, Docker or sleeps.

## Rules and hand-worked examples

### R1 — Catalog lookup and rule identity are exact

MOCK uses the frozen 175-market catalog, explicit venue symbols/native asset IDs,
and the matching rule profile. `BTCUSDT` resolves; `btcusdt`, `BTC/USD` and unknown
symbols must not be guessed into it. BTC price/size increments are 0.00000001,
minimum size 0.00000001 and minimum notional USD 10. Example: BUY 0.1 at 100
is exactly USD 10 and admissible; 0.09999999 at 100 is USD 9.999999 and not.
Catalog product/settlement, venue symbol and native ID must agree, not just asset.

### R2 — Admission rejects malformed or off-grid orders without fills

Positive finite price/quantity, exact product/instrument/venue context and supported
flags are mandatory. Off-grid quantity 0.200000001 or price 100.000000001 must
reject instead of rounding to 0.2/100. Zero, negative, NaN/infinity and post-only
IOC reject. Each item returns a canonical error; rejected items have no active
order, lifecycle fill or money movement. Mixed batches evaluate items separately:
one valid BUY 0.2 and one below-minimum BUY 0.01 at 100 admit only the first.

### R3 — Request idempotency and historical order identity prevent duplicates

Same request ID and payload returns the cached operation result without another
accepted update or order. Same ID with changed quantity rejects at operation scope.
Local order IDs and nonempty client IDs remain reserved even after cancel/fill;
a new request cannot reuse them for another economic order. Example: retry BUY
0.2 request `submit-1` ten times still represents 0.2, never 2.0. Client IDs alone
are not request deduplication. Operation result precedes accepted lifecycle events.

### R4 — Cancel/modify preserve real progress and cannot rewrite executed quantity

Cancel emits CancelPending then Canceled and prevents later fills of the remainder.
Modify revalidates admission and preserves local/native identity and cumulative
fills. Example: BUY 0.2 partially filled 0.1, resized to 0.3, has cumulative 0.1
and remainder 0.2. A replacement quantity 0.05 below already-filled 0.1 must reject
without rewriting the original request or progress (assumed safety rule). It cannot
change BTC identity to ETH or bypass minimum/grid rules. Lifecycle bridge updates
are not economic fills and cannot synthesize cash or accepted Fill events.

### R5 — Only admitted, active orders match, at or after activation

No orders means no fills. A rejected BUY below USD 10 cannot subsequently match.
An accepted BUY 0.2 active from 20 cannot fill at event 19; a touched event 20 may
fill. Matching uses only supplied observations, not a later bar or wall clock.
Fill events precede the resulting lifecycle update. Matching alone emits no fee
AccountingEvent; the account supplies actual fee postings.

### R6 — Limit touch, favorable opens and post-only rules are side-correct

BUY touches when low <= limit; SELL when high >= limit. BUY limit 100 with open
101/low 99 fills intrabar at 100; SELL limit 100 with open 99/high 101 likewise.
With no touch GTC rests and IOC cancels, both without fills. Post-only compares
against synthetic arrival open: BUY limit >= open or SELL limit <= open rejects
as PostOnlyWouldCross. These are OHLCV synthetic conventions, not order-book truth.

### R7 — Shared bar liquidity, partial fills and IOC never duplicate units

Default capacity is volume×10%, floored to size-grid units. A bar volume 1 permits
0.1 total units, shared across same-asset orders in ascending local order ID even
if submitted in reverse order. Orders BUY 0.2 IDs 1/2 give ID 1 only 0.1 and ID 2
zero on that bar. Two such bars can complete ID 1 at exactly 0.2; later bars cannot
fill it again. A 0.2 IOC on volume 1 fills 0.1, emits partial update then canceled
remainder; no future bar can execute the other 0.1. Filled plus remaining quantities
must stay consistent with request size. Zero-capacity bars cannot create fills.

### R8 — Latency and bounded slippage are deterministic

Latency counts eligible same-asset observations. With latency_events=1, the first
eligible BTC observation defers and the second can fill; an intervening ETH bar
must not advance BTC latency. No sleeps. Default maximum adverse slippage 250 ppm
means BUY at favorable open 99 with limit 100 fills between 99 and 99×1.00025
(99.02475), and never above limit 100; SELL with open 101/limit 100 fills between
101×0.99975 (100.97475) and 101. Prices stay on-grid. Same seed, prior state,
order IDs and event timestamps reproduce prices, quantities and native fill IDs.

### R9 — Reduce-only may reduce held units but never create/increase/reverse exposure

The flag is advertised by MOCK. End-to-end enforcement must uphold its economic
meaning (assumed because the frozen admission spec defers enforcement to accounting).
From flat, reduce-only BUY 0.2 must not create +0.2. From held +0.2, reduce-only
SELL 0.3 may reject or execute at most 0.2 and flatten, never create short −0.1.
Tests permit rejection or clamping, asserting the position/fill invariant rather
than inventing an undocumented reject phase or rounding policy.

### R10 — Fixed-point arithmetic preserves exact decimal economics or rejects

At scale 8, `0.2` and `2e-1` mean exactly 20,000,000 units; nonzero ninth digits,
negative unsigned values and overflow reject. Min-notional comparison is integer:
price 100 (10,000,000,000 units) × quantity 0.1 (10,000,000 units) meets USD 10,
while quantity 0.09999999 does not. Products/division round half up: 5/2 -> 3,
4/2 -> 2. USD 20 ×400 ppm gives USD 0.008 = 800,000 money units; rounding a
half-unit positive fee yields 1 unit. Weighted average 2 at 100 plus 1 at 160 is
120, even though scaled intermediate products exceed uint64. Overflow/zero
denominator return false rather than wrapped monetary values. Signed formatting
retains −0.00000001 and exact scale without locale or binary drift.

### R11 — Perpetual cash changes by realized PnL and fees, not principal

BUY 0.2 at 100 opens +0.2: notional 20, fee 0.008, cash 99,999.992, realized zero.
At mark 110 unrealized +2, equity 100,001.992; 1x margin is 22, available margin
99,979.992. BUY another 0.2 at 200 averages entry 150 with quantity 0.4. SELL
0.1 at 180 realizes (180−150)×0.1=3, preserves entry 150 and leaves +0.3;
total fees 0.008+0.016+0.0072=0.0312 and cash 100,002.9688.
SELL 0.2 at 100 then BUY 0.5 at 80 realizes +4 on 0.2 and flips to long +0.3
at entry 80; total fees 0.008+0.016=0.024, cash 100,003.976.
At leverage 2, marked gross 22 uses margin 11; leverage 0/21 rejects unchanged.

### R12 — Fill/cost identities apply once and conflicts make accounting unsafe

Duplicate native fill ID with identical payload does not repeat +0.2 or its 0.008
fee. Changed quantity/price under the same ID rejects, preserves economic state and
sets accountingSafe=false. Fee posting is one signed negative event, correlation
`fee:<fill ID>`, amount `-0.00800000`, settlement USD. Explicit rebate +1 and
funding −0.25 add +0.75 to cash; duplicate named cost IDs are ignored; conflicting
reuse marks unsafe. Off-grid marks/fills or mismatched native instrument identity
reject without changing prior economics.

### R13 — Durable replay restores full economics and matching/idempotency state

Admit BUY 0.2, volume-1 bar fills 0.1, checkpoint, then a second bar fills 0.1.
Cash is 99,999.992, quantity +0.2, two distinct deterministic fills. Restart from
the same directory must preserve account, order cumulative 0.2, stream/economic
fingerprints, dedup and sequence. Journal-only restart has the same guarantee.
Replaying a known old request or exact old source cannot add units/fees; corruption
or a truncated checkpoint/journal must fail closed rather than recover empty state.

### R14 — Source time, identity, stream cursors and snapshot merge stay explicit

Sequence starts at 1 and strictly orders all emitted events, including same-time
ones. Snapshot at S followed by streamAfter(S,n) returns only sequence>S and cursor
is the last delivered sequence. Pages are bounded by 256; all pages joined equal
the canonical stream, and querying its tail is empty. Snapshot reads add no stream
events. Same asset/time/payload bar is DuplicateIgnored even when old; changed
payload at same identity makes recovery unsafe without reapplying cash. New event
29 after accepted event 30 is OutOfOrderRejected and makes checkpoint unsafe.
Request retries may use their original older time. Example: snapshot after first
0.1 fill, then another 0.1 fill; incremental suffix contains just the later fill
and its accounting/lifecycle evidence, not the first fill again.

### R15 — Recovery codec preserves command economics and detects incomplete bytes

The codec is explicit little-endian with exact double bits and length-framed data.
Order ID 0x01020304 serializes u32 bytes 04 03 02 01; boolean byte 2 is invalid.
A submit containing quantity 0.2, limit 100, IOC and explicit identity must round-trip
unchanged; cancel/modify/bar retain the same business identities. A one-byte blob
or embedded-NUL client ID must not be silently shortened. Truncated scalar/string/
frame data throws. Appending two durable frames then reading returns the two payloads
in order; a declared frame length larger than remaining bytes cannot be ignored.
Checkpoint corruption and codec corruption are tested through public file readers.

### R16 — Reconciliation compares independent ledger evidence, and gates new submits

A complete matching stream/snapshot at the same sequence is CLEAN. Cash equals
initial 100,000 + realized − fees + funding/rebates. For a 0.1 fill at 100, fee
0.004 leaves 99,999.996; two ledger entries (fill, fee) link to the genesis hash.
A USD 1 cash drift, wrong position/order quantity or changed fill price is BLOCKED.
Changing an entry hash or removing its required fee evidence cannot produce CLEAN.
Different snapshot/local sequence is PENDING, never a guessed pass. Unsafe recovery
is BLOCKED. New submits require fresh CLEAN at the current sequence; any stream
advance invalidates that permission. Read-only comparison cannot repair cash or
mutate authoritative venue orders. Marks/leverage/equity are not independently
reconstructed by this stream-only reconciliation contract.

### R17 — Ambiguous and delayed responses preserve underlying venue truth

A lost submit response may leave one accepted BUY 0.2 but must block blind retry.
After CLEAN reconciliation retrying the same request creates no second order. A
delayed response is released by explicit poll/release, not a timer. Through the
public MockExchange facade, the operation receipt may be withheld while accepted
lifecycle evidence already exists; release does not apply another submission. These
faults cannot create a fill without a subsequent eligible bar.

### R18 — Connectivity, event-time limits and delivery faults cannot invent economics

Disconnect blocks new submits but existing BUY 0.2 may still fill 0.1 at event 20;
reconnect/backfill requires CLEAN before new exposure. Venue unavailable or stale
snapshot yields PENDING reconciliation, never fallback. The default submit budget
is two per 10 supplied event-time units: commands at 10/11 consume it; a third at
12 is retryably RateLimited after fresh reconciliation. At event 20 capacity resets.
Duplicate/reordered observed user-stream pages affect delivery only, never the
canonical sequence, stream fingerprint, cash or filled quantity. Fault decisions
and evidence are deterministic for the same explicit fault sequence/config/inputs.

## Invariants and limitations

- Admission/status updates are not fills; every fill has an accepted local order,
  correct instrument, positive grid-valid size and side, and respects its limit.
- Total filled size cannot exceed the original/current valid order quantity.
- Perpetual cash + unrealized PnL equals equity; fill fee postings occur once.
- Restart reproduces the same state and identities, not just a similar balance.
- Safety checks fail closed on ambiguous/conflicting/corrupt evidence.
- All timing uses explicit observations. Synthetic OHLCV does not claim L2 fidelity.
- No production changes, live venue calls, deployment or release-gate changes belong here.

## Findings

### F1 — Modification can reduce request size below already-filled quantity (R4)

Disabled test: `MockAdmission.DISABLED_R4_ModifyCannotShrinkBelowAlreadyExecutedQuantity`.
BUY 0.2 at limit 100 fills 0.1 on a volume-1 bar. Modifying it to quantity 0.05
and limit 200 (still USD 10 minimum notional) returns an accepted item, replaces
request quantity with 0.05 and sets remainder to zero while cumulative filled stays
0.1. R4's assumed safety rule requires rejecting that amendment and retaining the
original quantity 0.2 and remainder 0.1. `MockOrders::processModifyItem` reruns
admission but does not compare replacement quantity against cumulative fills;
it assigns the replacement then clamps the calculated remainder to zero.

Fixed (2026-10-19): Modifications below cumulative filled quantity reject before changing the order or reserving a replacement client ID.
Review (2026-10-20): a modify to exactly the filled quantity was still accepted and left a
live order with zero remaining; it is now rejected too
(`R4_ModifyToExactlyFilledQuantityIsRejectedAndKeepsTheRemainder`).

### F2 — Reduce-only is admitted but does not constrain matching exposure (R9)

Disabled tests:

- `MockVenueSafety.DISABLED_R9_ReduceOnlyOrderFromFlatCannotCreateExposure`
- `MockVenueSafety.DISABLED_R9_OversizedReduceOnlyExitCannotFlipLongToShort`

Through real `MockRecovery`, including admission, matching and account projection:

- From flat, reduce-only BUY 0.2 at 100 fills all 0.2, opens +0.2 and charges
  USD 0.008, reducing cash from 100,000 to 99,999.992 (9,999,999,200,000 scaled units).
- After a normal BUY opens +0.2, reduce-only SELL 0.3 fills all 0.3 and leaves
  short -0.1. The expected maximum exit fill is 0.2, with no negative remainder.

R9 deliberately allows rejection or clamping, so neither failure depends on an
invented rejection phase. Source inspection finds the flag checked for declared
rule support at admission and retained in request/codec state, but no economic
reduce-only check in the matching/account pipeline. This disagrees with R9's
explicitly assumed enforcement of the advertised capability. The original
assertions remain unchanged; production code is untouched.

Fixed (2026-10-19): Matching cancels reduce-only remainders without sufficient opposite-side account holdings, checking current holdings before each fill.

### Test registration correction and validation scope

The pre-run commit contained 23 tests in `admission_test.cpp` and
`matching_test.cpp`, but its `meson.build` was still a placeholder. Consequently,
the initially requested `ninja -C build -j2` reported no work and compiled no MOCK
tests. Step 3 corrects that test-registration mistake by registering those existing
sources as `test_unit_mock_exchange` in this area's Meson file; no new test bodies
or changed economic expectations were introduced after the pre-run commit.

These 23 tests cover catalog/admission, request identity, cancel/modify, matching,
capacity, timing/slippage and the two end-to-end reduce-only invariants. Dedicated
decimal, accounting, recovery/codec, reconciliation and fault-simulation tests
described by R10–R18 were not present in that commit and remain unvalidated by
this suite. The spec describes intended behaviour, not evidence of complete coverage.

Validation (WSL, 2026-10-19): after registration,
`ninja -C build -j2` compiled and linked without warnings.
`ALGOTRADING_SOURCE_ROOT="$PWD" build/tests/unit/mock_exchange/test_unit_mock_exchange --gtest_brief=1`
ran 23 tests: 20 passed and 3 failed, covering F1/F2. No assertion/API mistakes
or specification corrections were identified in the failing tests. Their bodies
are retained with `DISABLED_` names and `// FINDING:` comments.

After triage, `ninja -C build -j2` rebuilt without warnings and the same direct
execution command exited 0: **23 total, 20 passed, 3 disabled, 0 active failures**.
