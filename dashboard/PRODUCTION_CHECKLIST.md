# Production checklist — Step 30

Before making the dashboard reachable from the Internet:

- [ ] Host OS is patched and Docker Engine/Compose are current enough for the deployment.
- [ ] DNS points `DASHBOARD_DOMAIN` to the intended VPS/EC2 address.
- [ ] Only TCP 80/443 are public for the dashboard.
- [ ] SSH, if enabled, is restricted to an administrative IP/VPN.
- [ ] No host port is published for `dashboard-api`.
- [ ] PostgreSQL, NATS and trading services are not exposed publicly by this deployment.
- [ ] `.env.production` is mode 600 or otherwise appropriately protected and is not committed.
- [ ] Viewer and operator passwords are long, unique and different.
- [ ] `DASHBOARD_AUTH_ALLOW_DEMO=false` and Secure cookies are enforced by production Compose.
- [ ] `DASHBOARD_DATA_PROVIDER=real`.
- [ ] PostgreSQL DSN is correct; preferably use a dedicated read-only DB role.
- [ ] `ALGOTRADING_LIVE_NETWORK` identifies the actual running trading Docker network.
- [ ] Market-data host directory and DB file paths are absolute and correct.
- [ ] Canonical `database.db` is over-mounted read-only inside dashboard-api.
- [ ] Caddy certificate volumes are persistent.
- [ ] Root filesystems are read-only and `no-new-privileges`/capability drops remain present.
- [ ] CPU/RAM/PID limits and Docker log rotation are active.
- [ ] `./scripts/production-preflight.sh .env.production` passes.
- [ ] Caddy obtains a trusted TLS certificate and HTTP redirects to HTTPS.
- [ ] `/health` and `/api/health` work over HTTPS.
- [ ] `/api/overview` returns 401 before login.
- [ ] Viewer login succeeds and the session cookie is HttpOnly + Secure + SameSite=Strict.
- [ ] `/api/source-status` reports real mode and expected sources reachable.
- [ ] `/api/diagnostics` is authenticated and works.
- [ ] `/api/stream` establishes an SSE connection through Caddy.
- [ ] VIEWER cannot progress operator-only Manual Control actions.
- [ ] OPERATOR can validate/preview Manual Control but cannot route a real trading action.
- [ ] Browser bundle contains no passwords, PostgreSQL credentials, NATS credentials or exchange secrets.
- [ ] PostgreSQL and market-data backups are defined and restoration has been tested separately.
- [ ] Trading continues if dashboard-web and dashboard-api are stopped.
- [ ] `./scripts/production-smoke.sh .env.production` passes after deployment.

Before any future state-changing LIVE controls:

- [ ] Re-audit authorization per endpoint.
- [ ] Add strong identity/MFA appropriate to the deployment.
- [ ] Persist human-action audit durably.
- [ ] Route every control through the normal risk/execution business pipeline.
- [ ] Never add a browser-to-exchange path.
