# Step 35.1 — Explicit Symbol Mapping / Unsupported Classification

Step 35 closes the symbol-identity gap between canonical Binance-USDT market symbols and Hyperliquid TESTNET without pretending every market-data asset is executable on the selected venue.

Every current strategy symbol must be explicitly classified as VALID, UNSUPPORTED_EXPLICIT, UNSUPPORTED_VENUE_ABSENT, or MISSING_MAPPING. MISSING_MAPPING remains a hard failure. VALIDATED_WITH_UNSUPPORTED is a successful classification result, not full execution coverage. executionCoverageComplete=false remains fail-closed.

Explicitly blocked current symbols: AKEUSDT, BROCCOLI714USDT, NILUSDT, NOMUSDT, SAGAUSDT, XAIUSDT, 龙虾USDT.

Mappings whose Hyperliquid coin is absent from live TESTNET metadata are also blocked dynamically (the observed run included ENA, LSK, ONE and XRP).

Safety rule: UNSUPPORTED / UNMAPPED => NO ORDER ROUTING. No suffix stripping, case conversion, alias guessing, private auth, signing, account reads, order submission/cancel or real capital.

## Runtime acceptance update — 2026-09-25

Step 35.2 was executed successfully after the startup-readiness retry fix.
Observed current canonical universe classification:

- required symbols: 20
- supported/mapped on current Hyperliquid TESTNET: 9
- explicitly blocked / venue-absent: 11
- unclassified symbols: 0
- `executionCoverageComplete=false` by design
- unsupported symbols remain non-routable

Step 35 is therefore CLOSED / PASS. This does not enable private auth or orders.
