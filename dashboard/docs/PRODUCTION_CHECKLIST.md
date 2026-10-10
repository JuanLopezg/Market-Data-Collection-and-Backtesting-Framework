# Dashboard production checklist

Run script commands from `dashboard/`. See [AWS_DEPLOYMENT.md](AWS_DEPLOYMENT.md)
and [AUTH_SECURITY.md](AUTH_SECURITY.md).

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
- [ ] Install the host receiver/cleanup units and add the dashboard logging overlay; verify VPS logs survive restart/recreation, retain today/yesterday and trim older entries at 50 MiB per daily file; see [VPS log retention](../../deploy/live/README.md#vps-log-retention).
- [ ] `./scripts/production-preflight.sh .env.production` passes.
- [ ] Caddy obtains a trusted TLS certificate and HTTP redirects to HTTPS.
- [ ] `/health` and `/api/health` work over HTTPS.
- [ ] `/api/overview` returns 401 before login.
- [ ] Viewer login succeeds and the session cookie is HttpOnly + Secure + SameSite=Strict.
- [ ] `/api/source-status` reports real mode and expected sources reachable.
- [ ] `/api/diagnostics` is authenticated and works.
- [ ] `/api/stream` establishes an SSE connection through Caddy.
- [ ] VIEWER cannot progress operator-only Manual Control actions.
- [ ] OPERATOR can preview/admission-audit Manual Control with `submitted=false`; private submission stays disabled.
- [ ] Alert acknowledgements bind to the current lifecycle and do not resolve blockers.
- [ ] Watchdog, audit, acknowledgement and notifier data volumes are durable.
- [ ] Browser bundle contains no passwords, PostgreSQL credentials, NATS credentials or exchange secrets.
- [ ] PostgreSQL and market-data backups are defined and restoration has been tested separately.
- [ ] Trading continues if dashboard-web and dashboard-api are stopped.

The isolated local `PAPER_RECOVERY_TEST=1` rehearsal is documented in
[paper operations](../../deploy/paper_trading/README.md#local-backup-and-recovery-rehearsal).
It does not mark the deployed backup/restore or dashboard-independence items above
complete: accept those again on the destination host with its own state/configuration.
- [ ] `./scripts/production-smoke.sh .env.production` passes after deployment.

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
Unchecked roadmap requirements are not completed by passing deployment preflight.

Pending Trades/cost reporting acceptance (see the ordered roadmap):

- [ ] Show separate Commission and Funding columns in Trades with USD values, explicit costs/receipts and unavailable evidence.
- [ ] Show net PnL after commissions/funding; gross trade PnL is optional. Reconcile attribution and totals with account history and Perpetuals/Costs without double-counting.
- [ ] Accept actual funding-event timestamps/currencies, duplicate/restart handling and costs versus receipts before treating the display as real accounting.
- [ ] Gather the user's next dashboard adjustments before defining or implementing them.

These requirements are recorded on 2026-10-10; this update implements no UI or accounting changes.
