# Dashboard authentication and authorization

Implementation: `dashboard-api/internal/server/auth.go` and `server.go`.
The deployment boundary is documented in [AWS_DEPLOYMENT.md](AWS_DEPLOYMENT.md).

## Credentials and sessions

Copy `dashboard/.env.production.example` to `dashboard/.env.production`
and replace placeholders with long, unique viewer/operator passwords. Never commit
real credentials. Production Compose enforces `DASHBOARD_AUTH_ALLOW_DEMO=false`,
Secure cookies and a bounded session lifetime (default `8h`).
The API fails closed when non-demo credentials are not configured.

Login generates an opaque random session token and separate CSRF token. The
`cd_session` cookie is HttpOnly, SameSite=Strict, path `/`, bounded by
expiry and Secure in production. Sessions live in server memory: API restart invalidates
them. Durable alert/audit records are stored separately and survive session loss.

Failed logins are throttled in memory by originating client address. Production exposes
Caddy only; the API has no public host port.

## Roles and CSRF

VIEWER may read authenticated resources. OPERATOR may invoke manual preview, manual
route admission and alert acknowledgement. Role checks occur server-side.
State-changing requests, including logout, require the session's
`X-CSRF-Token`. Inputs are bounded and validated server-side.

Manual preview does not change trading state. Route admission binds the request/reference
and durably records operator intent, but reports `submitted=false` and routing
disabled. This is an audit boundary, not authorization to submit a private order.
Acknowledgements bind to exact active durable alert lifecycles; they do not resolve
alerts, enable trading or alter positions.

## Source and secret boundary

The browser never receives SQL/NATS credentials, exchange secrets or session identifiers
in frontend state. `VITE_*` settings are public bundle data.
Real trading reads are bounded/read-only; isolated manual-audit and acknowledgement
writes do not grant trading database mutation.

Public Hyperliquid TESTNET probes use fixed public metadata/mid-price bodies, no key or
signature. The configured HTTPS endpoint is validated, redirects disabled, and requests
go through the authenticated API rather than directly from the browser.

Before future capital-moving LIVE controls, revisit per-endpoint authorization and
appropriate strong identity/MFA. Private signing/control delivery remains separate
unfinished work; production dashboard deployment does not enable it.

## Optional phone access (cancelled task)

The user cancelled mobile dashboard access on 2026-10-09; activation and handset
acceptance are removed from pending requirements. The retained optional
[PAPER mobile runbook](../../deploy/paper_trading/README.md#optional-temporary-mobile-access-cancelled-task)
provides a PC-side, single-phone HTTPS bridge to an existing loopback dashboard or
VPS SSH tunnel. It preserves API authentication, roles, CSRF and SSE; it is not a
new authorization layer. Its IP allowlist is an additional connection restriction,
not user identity. Verify the self-signed certificate fingerprint before trusting
it. No credentials/requests are logged and generated keys remain ignored.
The upstream remains loopback HTTP with its existing cookie settings; the phone
listener accepts TLS only. Its transport tests do not establish handset acceptance;
no public web/VPN deployment is scheduled for this cancelled task.
