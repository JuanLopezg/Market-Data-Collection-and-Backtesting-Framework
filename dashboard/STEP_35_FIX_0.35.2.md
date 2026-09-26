# Step 35.2 — startup readiness race fix

Observed failure after a successful `docker compose ... --force-recreate`:

- `control-dashboard-api` reported `Healthy`.
- `control-dashboard-web` reported `Started`.
- the immediate Step 35 precondition entered Step 34 -> Step 33.
- the first POST to `/api/auth/login` failed with `curl: (56) Recv failure: Connection reset by peer`.

This is a startup/readiness race at the Caddy/web edge, not a symbol-mapping or Hyperliquid contract failure.

## Fix

`step33-testnet-foundation.sh` now retries only the dashboard login transport/startup condition:

- default 15 attempts;
- 2 seconds between attempts;
- retries curl transport failures and HTTP 502/503/504 only;
- a real non-200 authentication/configuration response fails immediately;
- after the retry budget is exhausted it fails closed.

Environment overrides:

- `STEP33_LOGIN_ATTEMPTS`
- `STEP33_LOGIN_RETRY_SECONDS`

No trading, auth, venue, mapping, safety-gate, or routing semantics were weakened.

Step 35.1 explicit symbol classification semantics remain unchanged:

- every current strategy symbol must be explicitly mapped or explicitly classified unsupported;
- unsupported and venue-absent assets remain non-routable;
- no heuristic mapping;
- no private auth, signing, submit/cancel, or capital.
