# Dashboard production checklist

Use [chain operations](../../deploy/chain/README.md) for installation, startup,
private access and releases, and [AUTH_SECURITY.md](AUTH_SECURITY.md) for API controls.
The deployed dashboard uses loopback HTTP through an SSH tunnel or private Tailscale
HTTPS Serve (Funnel disabled).

Before accepting dashboard access on the chain host:

- [ ] Host OS is patched and Docker Engine/Compose are current enough for the deployment.
- [ ] `dash-web` binds to loopback only; access uses the intended SSH tunnel or permitted tailnet.
- [ ] SSH, if enabled, is restricted to an administrative IP/VPN.
- [ ] No host port is published for `dash-api`.
- [ ] PostgreSQL, NATS and trading services are not exposed publicly by this deployment.
- [ ] `<root>/secrets/chain.env` is mode 600, its parent is mode 700, and credentials are not committed or printed.
- [ ] Viewer and operator passwords are long, unique and different.
- [ ] `DASHBOARD_AUTH_ALLOW_DEMO=false`; verify the actual cookie policy (chain currently sets `DASHBOARD_AUTH_COOKIE_SECURE=false` behind private access).
- [ ] `DASHBOARD_DATA_PROVIDER=real`.
- [ ] PostgreSQL DSN is correct; preferably use a dedicated read-only DB role.
- [ ] The installed PAPER/LIVE mode selects the intended separate market, database, message and dashboard state paths.
- [ ] The market-data directory is mounted read-only inside `dash-api` and `watchdog`.
- [ ] Watchdog/notifier read-only filesystems, `no-new-privileges` and capability drops remain present.
- [ ] Configured CPU/RAM/PID limits, host logging and bounded Docker log caches are active.
- [ ] Chain host receiver/cleanup units are installed; verify logs survive restart/recreation, retain today/yesterday and trim older entries at 50 MiB per daily file; see [chain operations](../../deploy/chain/README.md).
- [ ] Private Tailscale HTTPS access uses a trusted certificate with Funnel disabled; verify the tailnet access policy.
- [ ] `/health` and `/api/health` work over HTTPS.
- [ ] `/api/overview` returns 401 before login.
- [ ] Viewer login succeeds and the session cookie is HttpOnly + SameSite=Strict with the configured Secure setting.
- [ ] `/api/source-status` reports real mode and expected sources reachable.
- [ ] `/api/diagnostics` is authenticated and works.
- [ ] `/api/stream` establishes an SSE connection through the private access path and Caddy.
- [ ] VIEWER cannot progress operator-only Manual Control actions.
- [ ] OPERATOR preview and confirmation/audit work with the configured chain request folder; blocked admission stays `submitted=false`. Private submission requires its separate explicit gate/authorization.
- [ ] Alert acknowledgements bind to the current lifecycle and do not resolve blockers.
- [ ] Watchdog, audit, acknowledgement and notifier state directories are durable and separate from trading truth.
- [ ] Browser bundle contains no passwords, PostgreSQL credentials, NATS credentials or exchange secrets.
- [ ] PostgreSQL and market-data backups are defined and restoration has been tested separately.
- [ ] Trading continues if `dash-web` and `dash-api` are stopped.

The isolated local `PAPER_RECOVERY_TEST=1` rehearsal is documented in
[validation recovery guide](../../validation/README.md).
It does not mark the deployed backup/restore or dashboard-independence items above
complete: accept those again on the destination host with its own state/configuration.

Before any future state-changing LIVE controls:

- [ ] Re-audit authorization per endpoint.
- [ ] Add strong identity/MFA appropriate to the deployment.
- [ ] Verify implemented manual-intent and alert-acknowledgement stores survive restart and remain separate from trading truth.
- [ ] Route every control through the normal risk/execution business pipeline.
- [ ] Never add a browser-to-exchange path.

Remaining product and live acceptance is ordered in the
[project roadmap](../../docs/ROADMAP.md), including chart/timeframe behavior,
Pipeline/Risk/Infrastructure completeness, funding/BTC collateral, real Telegram
delivery, Manual Control after venue integration, the simplest fast-backtest
performance baseline, and CPU/RAM profiles during complete backtests and live trading.
Unchecked roadmap requirements are not completed by passing dashboard access checks.

Pending Trades/cost reporting acceptance (see the ordered roadmap):

- [ ] Show separate Commission and Funding columns in Trades with USD values, explicit costs/receipts and unavailable evidence.
- [ ] Show net PnL after commissions/funding; gross trade PnL is optional. Reconcile attribution and totals with account history and Perpetuals/Costs without double-counting.
- [ ] Accept actual funding-event timestamps/currencies, duplicate/restart handling and costs versus receipts before treating the display as real accounting.
- [ ] Gather the user's next dashboard adjustments before defining or implementing them.

These requirements are recorded on 2026-10-10; this update implements no UI or accounting changes.
