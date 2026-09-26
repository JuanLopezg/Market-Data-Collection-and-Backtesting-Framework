# Step 45 fix 0.45.4

The Step 45 gate correctly requires the REAL projection source note to disclose that PnL, slippage and accepted replay baselines remain unavailable/deferred.

Under insufficient canonical history the contract was already honest in the structured `coverage` rows, but the short `sourceNote` used the generic word `execution` instead of explicitly naming `slippage`. The gate therefore failed at `[4/8]` even though `[3/8]` correctly validated the insufficient-history state.

This fix changes only the disclosure text for the insufficient-history branch. It explicitly names PnL/equity, slippage, fill latency, reject rate, execution-distribution and accepted-replay historical baselines as not inferred. It does not fabricate a baseline, lower the six-observation requirement, enable routing, or change any trading semantics.
