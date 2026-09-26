# Step 30 — AWS / VPS production hardening

Step 30 packages the dashboard for a small production VPS/EC2 deployment without changing the trading business path.

## What changes

- Production Caddy remains the only public container: TCP 80/443.
- `dashboard-api` has no host-published port and remains behind Caddy.
- Production real-data overlay joins the existing trading Docker network only for PostgreSQL/NATS access.
- Canonical `database.db` is over-mounted read-only; the containing directory remains available for SQLite WAL/SHM bookkeeping.
- Root filesystems remain read-only, Linux capabilities are dropped, `no-new-privileges` is enabled, and CPU/RAM/PID/log limits remain bounded.
- Demo credentials remain disabled and cookies are forced Secure + HttpOnly + SameSite=Strict by the API.
- Caddy terminates TLS, redirects HTTP to HTTPS, applies HSTS/CSP/security headers, and preserves unbuffered SSE.
- Production preflight now fails closed on example credentials, mock provider, missing market-data paths, missing live Docker network, missing private-network boundary, or invalid Compose.
- Production smoke verifies HTTPS, unauthenticated 401, authenticated real source status, diagnostics and the SSE handshake.
- A systemd unit example is included for reboot-safe operation.

## Files added / updated

- `docker-compose.production.yml`
- `docker-compose.production.real.yml`
- `.env.production.example`
- `Caddyfile.production`
- `scripts/production-preflight.sh`
- `scripts/production-smoke.sh`
- `deploy/control-dashboard.service.example`
- `AWS_DEPLOYMENT.md`
- `PRODUCTION_CHECKLIST.md`

## Safety boundary

Step 30 does **not** expose PostgreSQL, NATS, dashboard-api or trading-service ports to the Internet. It does not enable manual trading routing, exchange submission, pause/resume or a kill switch. Dashboard failure still must not stop trading.

## Production launch shape

Start the trading stack first, then from `dashboard/`:

```bash
cp .env.production.example .env.production
# edit every placeholder and host path

./scripts/production-preflight.sh .env.production

docker compose \
  --env-file .env.production \
  -f docker-compose.production.yml \
  -f docker-compose.production.real.yml \
  up -d --build --remove-orphans

./scripts/production-smoke.sh .env.production
```

Do not use the production compose on the public Internet until DNS, firewall/security-group rules, host patching, backups and secrets handling are reviewed.
