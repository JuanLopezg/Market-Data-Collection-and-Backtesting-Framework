# Step 37A fix v0.37A.3 — classify newly observed Binance/source symbols

## Failure observed

The live Step 37A gate correctly failed because the canonical top-50 market-data ranking changed after the original registry snapshot. Nine symbols were not present in the versioned registry:

- AEROUSDT
- ARKUSDT
- BRUSDT
- BTWUSDT
- LITUSDT
- LYNUSDT
- MUBARAKUSDT
- PHAUSDT
- RAREUSDT

Two of them (`MUBARAKUSDT`, `PHAUSDT`) were also present in the current Strategy universe, so the registry raised CRITICAL `SYMBOL_UNREGISTERED_STRATEGY` alarms and remained fail-closed.

## Correction

The nine observed Binance/source symbols are now explicit registry entries and explicit Step 35 classifications. They are deliberately `BLOCKED_EXPLICIT` for Hyperliquid TESTNET because no execution-symbol mapping has been manually reviewed and approved for them.

This is intentionally conservative: the fix does **not** infer `AERO -> AERO`, strip suffixes, perform fuzzy matching, or assume that a same-named Hyperliquid coin is economically equivalent.

Registry version:

`2026-09-26-step37a.3`

Catalog after the correction:

- 154 approved Hyperliquid TESTNET mappings
- 16 explicit blocked classifications
- 170 total cross-venue classifications

## Safety invariant

A new Binance/source symbol may appear in market data at any time. Until an operator explicitly classifies it in the versioned registry, Step 37A must raise a CRITICAL alarm and the symbol must remain non-routable.

`UNREGISTERED / BLOCKED_EXPLICIT / VENUE_ABSENT / DIVERGENT => NO ORDER ROUTING`
