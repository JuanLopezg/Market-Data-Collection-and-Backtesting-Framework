# Step 45 fix 0.45.2

The Step 45 Docker/frontend build is healthy, but the Step 44 precondition correctly failed closed because the live canonical Binance ranking drifted after Step 37A.3 and introduced two previously unclassified symbols: `2ZUSDT` and `QUSDT`.

This fix does **not** guess Hyperliquid aliases. It advances the explicit multi-exchange registry to `2026-09-26-step37a.4` and adds both assets as exact Binance/SOURCE identities with Hyperliquid TESTNET status `BLOCKED_EXPLICIT`, routing policy `DENY`, pending manual mapping review. The accepted Step 35 manifest is updated with the same two explicit blocked classifications so the registry and manifest remain identical.

The current observed ranking regression fixture is also refreshed: 50/50 ranking symbols classified, 20/20 strategy symbols classified, with 11 currently publicly-routable strategy symbols and 9 non-routable strategy symbols. Runtime venue absence remains dynamic and fail-closed.

No heuristic symbol conversion, wallet, private auth, signing, submit/cancel, or trading route is introduced.
