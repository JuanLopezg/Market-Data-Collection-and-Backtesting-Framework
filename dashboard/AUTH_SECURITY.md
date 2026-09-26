# Authentication & authorization notes

## Production credentials

Copy `.env.production.example` to `.env.production` and replace the placeholder passwords with long unique random values. Never commit the real file.

The production Compose configuration sets:

- `DASHBOARD_AUTH_ALLOW_DEMO=false`
- `DASHBOARD_AUTH_COOKIE_SECURE=true`
- `DASHBOARD_SESSION_TTL=8h` by default

The API will fail closed if no real dashboard credentials are configured.

## Session model

A successful login creates a cryptographically random opaque session token and a separate CSRF token. The opaque token is stored server-side and is sent to the browser only as the `cd_session` cookie.

Cookie policy:

- `HttpOnly`
- `SameSite=Strict`
- `Secure` in production
- path `/`
- bounded expiry

Sessions are in memory. API restart = all sessions invalidated.

## Roles

`VIEWER` is observation-only.

`OPERATOR` is intended for explicitly authorized operational actions. Step 12 exposes no real trading action yet; future POST/PUT/DELETE control routes must enforce OPERATOR server-side rather than trusting the UI.

## CSRF

The API issues a random CSRF token per session. State-changing requests must provide it in `X-CSRF-Token`. Logout already exercises this path.

## Brute-force protection

Failed logins are throttled in memory per originating client address. The API is not published directly to the Internet; Caddy is the only public entry point.

## Future production decision

Local dashboard credentials are sufficient to build and test the boundary, but before a real LIVE control plane is exposed publicly, decide whether to move identity to an external OIDC/SSO provider with MFA. That decision does not require changing the dashboard read-model architecture.

## Step 26 control-preview authorization

Manual portfolio preview is the first state-changing HTTP verb added after login, but it is intentionally **non-mutating with respect to trading state**. `POST /api/manual-control/preview` is protected by all of:

- HttpOnly SameSite session cookie;
- OPERATOR role check;
- CSRF token check;
- bounded request body;
- server-side CSV validation.

A VIEWER cannot invoke it. The endpoint does not publish commands or write trading state. The absence of a verified runtime manual-target contract keeps routing fail-closed.

## Step 34 public venue probe

The Hyperliquid TESTNET public probe uses no API key, private key, signature, wallet address or account endpoint. The configured URL is validated fail-closed to the exact HTTPS testnet `/info` endpoint, redirects are disabled, and only fixed public `meta` / `allMids` request bodies are emitted. Browser access remains through authenticated dashboard REST; the browser never contacts Hyperliquid directly.
