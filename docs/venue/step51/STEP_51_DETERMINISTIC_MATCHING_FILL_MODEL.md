# Step51 — Deterministic Matching / Fill Model

**Entering state:** Step50 PASS/CLOSED.  
**Bound Step50 lifecycle fingerprint:** `bf5e867c6793f7b9ad3c7487f6bdb2d95d296e827406a7c3870533833c09b1c3`

Step51 adds deterministic synthetic market matching and canonical `Fill` events for
MOCK. It does not claim historical order-book/L2 fidelity.

## Market model

Input is one OHLCV bar plus a business/event timestamp supplied by upstream replay or
TimeHandler semantics. The matcher itself never reads wall-clock or monotonic time.

Limit touch rule:
- BUY executes only when `bar.low <= limit`;
- SELL executes only when `bar.high >= limit`.

The model is explicitly `OHLCV_BAR_SYNTHETIC`.

## Shared synthetic liquidity

One bar has one shared liquidity pool:

`floor(bar.volume * participation)`, rounded down to the MOCK size increment.

Default participation is 10%. Orders for the same asset/bar consume that pool in
ascending `local_order_id`, making simultaneous allocation deterministic rather than
dependent on hash/map iteration order.

## Price and slippage

If the bar opens through a normal limit order, execution begins from the bar open and
applies deterministic adverse slippage derived from:

`seed + order id + event timestamp + per-order fill sequence`

Default maximum adverse slippage is 2.5 bps. The final price is quantized to the venue
price grid and can never be worse than the limit.

If the bar only touches the limit intrabar, the limit itself is the synthetic execution
reference.

## Post-only

Post-only uses the bar open as the explicit synthetic arrival reference:
- BUY limit >= open => `REJECTED / POST_ONLY_WOULD_CROSS`;
- SELL limit <= open => same.

This is a simulation convention, not historical L2 evidence.

## GTC / IOC

GTC:
- no execution => RESTING;
- partial => PARTIALLY_FILLED;
- full => FILLED.

IOC:
- no execution => CANCELED;
- partial => Fill, PARTIALLY_FILLED, then CANCELED remainder;
- full => FILLED.

## Fill identity and event ordering

Native MOCK fill IDs are deterministic:

`mock-fill-<localOrderId>-<perOrderFillSequence>`

For economic execution, event order is:

`Fill -> lifecycle update`

This preserves the project rule that an order is not a fill and that economic execution
comes from the Fill event.

## Fee model

Step51 config includes a deterministic fee rate (default 4 bps) and computes a diagnostic
fee quote for each synthetic fill trace. It deliberately does **not** emit an
`AccountingEvent`; durable fee/funding/accounting posting belongs to Step52.

## Latency

Latency is measured in same-asset market events, not wall-clock milliseconds. Default is
0 events. This keeps economic results independent of replay speed. A nonzero setting can
be used by deterministic tests without introducing real-time sleeps.

## Determinism

The Step51 fingerprint binds the exact Step50 lifecycle fingerprint plus the matching
model config. Same input events, same seed/config and same prior state must produce the
same fill IDs, prices, quantities, lifecycle transitions and diagnostic traces.

## Still deferred

- cash/equity/positions/PnL/margin state -> Step52;
- fee/funding AccountingEvents -> Step52;
- snapshots/user-stream/restart/backfill -> Step53;
- reconciliation/ledger parity -> Step54;
- chaos/rate-limit engine -> Step55;
- private Hyperliquid auth/signing/routing remains deferred until after Step62.

## Next

**Step52 — Mock Account / Margin / Positions / Accounting**
