# Step 26 — Manual Control backend foundation

Step 26 replaces the browser-only manual portfolio mock with an authenticated server-side **validation and target-delta preview**.

## What is real now

- `GET /api/manual-control` is available in real-provider mode.
- `POST /api/manual-control/preview` accepts a CSV payload only from an authenticated `OPERATOR` session with a valid CSRF token.
- CSV parsing/validation runs in `dashboard-api`, not in the browser.
- Validation checks the exact `asset,weight_pct` schema, row count, duplicates, finite 0–100 weights, 100% total, and asset membership against the canonical current market universe/current positions/current approved target set.
- The preview compares the requested portfolio with the latest durable PortfolioRisk-approved target and computes a deterministic SHA-256 request hash.
- The reference-capital target delta is shown server-side.

## What remains deliberately disabled

Step 26 **does not**:

- publish a NATS trading command;
- call an exchange;
- create or modify an ExecutionState order;
- claim that PortfolioRisk approved the manual request;
- fabricate venue quantities/fees;
- persist a durable human-action audit record;
- expose a submit/route endpoint.

This is intentional. The audited C++ runtime currently has no verified manual-target business contract. Routing around that missing contract would violate the dashboard architecture. A future control contract must re-enter the normal `PortfolioRisk -> OrderPlanner -> ExecutionState -> ExchangeGateway` path.

## Security boundary

`POST /api/manual-control/preview` requires:

1. valid HttpOnly session cookie;
2. `OPERATOR` role;
3. matching `X-CSRF-Token`;
4. bounded JSON body and CSV payload;
5. server-side validation.

`VIEWER` receives HTTP 403. Missing/invalid CSRF receives HTTP 403.

## Expected UI

Manual Control should now say `Fail-closed control foundation`. Uploading the generated demo CSV should produce a server-side PASS and allow navigation through Preview / Risk boundary / Target delta / Confirm. The final route button remains disabled by design.
