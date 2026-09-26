# Step 32 — LIVE Safety Pass / final pre-testnet safety gate

Step 32 is a **fail-closed safety boundary before testnet integration**. It does not add exchange credentials, signing, order submission, cancellation, pause/resume, kill-switch routing or any other capital-moving capability.

## What changed

- Added authenticated `GET /api/safety-gate`.
- Added a **Step 32 · Pre-Testnet Safety Gate** panel to Infrastructure.
- The gate correlates the current real-provider shell, Step 31 end-to-end proof and Manual Control routing state.
- It separates two concepts that must not be conflated:
  - `safeToProceed`: no verified contradiction blocks continued **testnet integration work**;
  - `tradingReady`: strict runtime readiness, still fail-closed.
- Added `scripts/step32-live-safety-gate.sh` for an explicit WSL/manual acceptance gate.
- Repaired the stale `dashboard-real-smoke` helper so the cumulative dashboard once again passes `go test ./...`, `go vet ./...` and `go build ./...`.
- Version bumped to `0.32.0`.

## Safety checks

The runtime endpoint checks:

1. real provider (mock mode cannot pass);
2. no verified runtime hard blocker (`PAUSED` blocks);
3. no active critical alert;
4. Step 31 proof is not `BLOCKED`;
5. Manual Control still reports `routeEnabled=false`;
6. local/production auth posture is surfaced (demo auth / non-Secure cookie are warnings in local WSL);
7. dashboard command surface remains read-first.

The CLI gate additionally audits the source tree for:

- current trading-contract markers;
- Go test/vet/build;
- absence of NATS publish operations in dashboard-api;
- absence of submit/cancel/pause/resume/kill/order HTTP routes;
- absence of mutating SQL in the dashboard PostgreSQL integration;
- production auth/network/container hardening;
- live `/api/safety-gate`, `/api/manual-control` and Step 31 `/api/pipeline` evidence.

## Verdict semantics

- `PASS`: no blocker and no warning.
- `WARN`: safe to continue integration, but local/deployment/runtime evidence remains incomplete. This is expected in the current WSL/pre-exchange environment.
- `BLOCKED`: stop. Resolve the reported contradiction/blocker before continuing.

`PASS` or `WARN` does **not** mean the system is authorized or ready to trade. `tradingReady` stays independently fail-closed.

## Local WSL acceptance

Start the normal local real stack as before, then run:

```bash
./scripts/step32-live-safety-gate.sh
```

Defaults assume the local demo operator (`operator / operator-demo`) and `http://localhost:8080`. Override without editing the script if needed:

```bash
DASHBOARD_GATE_USERNAME=operator \
DASHBOARD_GATE_PASSWORD='...' \
DASHBOARD_BASE_URL=http://localhost:8080 \
./scripts/step32-live-safety-gate.sh
```

Expected current result is commonly `WARN` rather than `PASS` because local HTTP/demo auth and incomplete exchange/service/clock readiness contracts are intentionally visible. The script accepts warnings but fails on verified blockers or contradictions.

## What Step 32 deliberately does not do

- no Binance/testnet adapter;
- no API keys or secrets;
- no signing;
- no private exchange endpoint;
- no order submit/cancel;
- no dashboard-generated reconciliation request;
- no manual routing;
- no claim that `DEGRADED` equals `READY`.


## v0.32.4 diagnostic correction

The Step 32 gate no longer reports a generic `PAUSED` blocker only. It now carries the verified `shell-status.blockers[]` and `readinessDetail` into `/api/safety-gate`, so a blocked gate identifies the concrete runtime dependency (PostgreSQL, NATS, missing ExecutionState state, market-data failure, or reconciliation BLOCKED). `PAUSED` is still fail-closed; this change does not weaken the safety gate.
