# AWS / VPS deployment — Step 30

The Control Dashboard is an observability/control layer. It must remain removable without affecting trading.

## Network boundary

```text
Internet
   |
   | TCP 80/443 only
   v
Caddy / dashboard-web
   |
   | dashboard-private (internal Docker network)
   v
dashboard-api
   |
   +---- PostgreSQL / NATS through algotrading-live Docker network
   |
   +---- canonical market-data SQLite bind mount
```

Never publish `dashboard-api`, PostgreSQL, NATS or trading-service ports from the dashboard deployment.

## VPS prerequisites

- Linux VPS/EC2 with Docker Engine + Compose plugin.
- Stable public IP; on AWS, an Elastic IP is preferable.
- DNS A/AAAA record for the dashboard hostname.
- Host time synchronization enabled.
- Current OS/security updates.
- Firewall/security group allowing 80/443 publicly and SSH only from an administrative IP/VPN if SSH is used.
- Trading stack already running on the same Docker host for this deployment model.
- Persistent backup strategy for PostgreSQL and the canonical market-data database before LIVE operation.

## AWS Security Group

Inbound:

| Port | Source | Purpose |
| --- | --- | --- |
| 443/TCP | intended dashboard users / Internet as required | HTTPS |
| 80/TCP | Internet | redirect + ACME |
| 22/TCP | admin IP/VPN only | SSH, if used |

Do not add public rules for 8080, 5432, 4222, 8222 or trading services.

Outbound must allow DNS and HTTPS so Caddy can obtain certificates. Internal Docker communication does not require public inbound rules.

## Configure

```bash
cd /opt/algotrading/dashboard
cp .env.production.example .env.production
chmod 600 .env.production
```

Edit every placeholder. `DASHBOARD_DOMAIN` is a hostname only, without `https://`.

For PostgreSQL, Step 30 accepts a DSN via `DASHBOARD_POSTGRES_DSN`. A dedicated SELECT-only dashboard role is preferable to reusing the trading writer role; creating that DB role is outside this dashboard step because it changes the database security model.

Set absolute host paths for:

```text
DASHBOARD_MARKET_DATA_HOST_DIR
DASHBOARD_MARKET_DATA_DB_HOST_FILE
```

The DB file itself is over-mounted read-only inside `dashboard-api`.

## Start the trading stack first

The real overlay expects an existing Docker network, default:

```text
algotrading-live_live
```

If your Compose project creates a different network name, set `ALGOTRADING_LIVE_NETWORK` in `.env.production`.

## Preflight

```bash
./scripts/production-preflight.sh .env.production
```

It fails closed on example credentials, mock mode, missing source paths, missing Docker network, invalid Compose or a public dashboard-api port.

## Deploy

```bash
docker compose \
  --env-file .env.production \
  -f docker-compose.production.yml \
  -f docker-compose.production.real.yml \
  up -d --build --remove-orphans
```

Check:

```bash
docker compose \
  --env-file .env.production \
  -f docker-compose.production.yml \
  -f docker-compose.production.real.yml \
  ps
```

Only Caddy should publish host ports.

## Smoke test

After DNS and TLS are live:

```bash
./scripts/production-smoke.sh .env.production
```

It verifies HTTPS health, API liveness, pre-login 401 protection, viewer login, real source status, diagnostics and an SSE handshake through Caddy.

## TLS

Caddy obtains/renews certificates automatically. `caddy-data` and `caddy-config` are named volumes so certificate state survives container replacement.

The production Caddyfile applies HSTS, CSP, clickjacking/mime/referrer/permissions headers and keeps `/api/stream` unbuffered for SSE.

## Reboot startup

`deploy/control-dashboard.service.example` is a systemd template. Review paths first, then install it deliberately if desired. Docker daemon access is root-equivalent, so do not grant Docker access casually just to run this unit.

## Secrets

- `.env.production` must never be committed.
- `VITE_*` values are public frontend build-time data and must never contain secrets.
- Use long, unique VIEWER and OPERATOR passwords.
- Before real capital-moving controls are added, replace/augment local password auth with an appropriate identity solution (for example SSO/OIDC + MFA) and re-audit authorization.

## Resource isolation

Initial limits remain intentionally small so the dashboard cannot crowd out trading:

- web/Caddy: 96 MB, 0.25 CPU, 96 PIDs.
- API: 64 MB, 0.20 CPU, 64 PIDs.

Step 29 diagnostics provide evidence to tune these later. Increase limits only from measurements.

## Still not a LIVE-capital approval

Step 30 hardens deployment. It does not prove the trading runtime is safe for live capital and does not complete the outstanding runtime replay/chaos gates, exchange integration, manual-control routing, ledger or full end-to-end safety campaign.
