# Dashboard authentication and authorization

Implementation: `dashboard-api/internal/server/auth.go` and `server.go`.
The deployment boundary is documented in [chain operations](../../deploy/chain/README.md).

## Credentials and sessions

The chain installer creates viewer/operator passwords in `<root>/secrets/chain.env`;
keep that file protected (0600, parent directory 0700) and never commit or print it.
Use long, unique and different passwords. Chain Compose sets
`DASHBOARD_AUTH_ALLOW_DEMO=false`; sessions have a bounded lifetime (default `8h`).
The API fails closed when non-demo credentials are not configured.

Login generates an opaque random session token and separate CSRF token. The
`cd_session` cookie is HttpOnly, SameSite=Strict, path `/`, bounded by
expiry. The chain uses loopback HTTP behind an SSH tunnel or private Tailscale HTTPS
Serve and currently sets `DASHBOARD_AUTH_COOKIE_SECURE=false`; do not assume the
cookie has the Secure attribute. Sessions live in server memory: API restart invalidates
them. Durable alert/audit records are stored separately and survive session loss.

Failed logins are throttled in memory by originating client address. Chain exposes
`dash-web` on loopback only; `dash-api` has no public host port.

## Roles and CSRF

VIEWER may read authenticated resources. OPERATOR may invoke manual preview, manual
route admission and alert acknowledgement. Role checks occur server-side.
State-changing requests, including logout, require the session's
`X-CSRF-Token`. Inputs are bounded and validated server-side.

Manual preview does not change trading state. Without a configured control sink,
route admission binds the request/reference and durably records operator intent,
but reports `submitted=false` and routing disabled. The chain configures a manual
request folder; accepted requests go through execution-state and its normal
risk/execution checks. See [Manual Control](../../deploy/chain/README.md#manual-control-paper-and-live)
for PAPER/LIVE behavior and the separate private-submission gate.
Acknowledgements bind to exact active durable alert lifecycles; they do not resolve
alerts, enable trading or alter positions.

## Source and secret boundary

The browser never receives SQL/NATS credentials, exchange secrets or session identifiers
in frontend state. `VITE_*` settings are public bundle data.
Real trading reads are bounded/read-only; isolated manual-audit and acknowledgement
writes do not grant trading database mutation.

Public venue reads (Kraken tickers for the Symbol Mapping page) use fixed public URLs, no
key or signature, and go through the authenticated API rather than directly from the browser.

Before future capital-moving LIVE controls, revisit per-endpoint authorization and
appropriate strong identity/MFA. Private signing/control delivery remains separate
unfinished work; production dashboard deployment does not enable it.

## Private phone access

The VPS uses [private Tailscale HTTPS Serve](../../deploy/chain/README.md#private-mobile-dashboard-access-tailscale)
with Funnel disabled. The phone must belong to the permitted tailnet and still
authenticate to the dashboard. API roles, CSRF and SSE are preserved. Device
certificate names appear in public certificate-transparency records; dashboard
content remains private. Review tailnet access policy before adding members.
