# Step 45 fix 0.45.1

Frontend compatibility fix only.

- Replaces `String.prototype.replaceAll` with `split(...).join(...)` in `LiveVsExpectedPage.tsx`.
- Keeps the TypeScript target/lib at ES2020; no compiler target broadening is required.
- No backend readiness, wallet, signing, routing, or trading semantics changed.
