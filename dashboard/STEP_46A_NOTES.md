# Step 46A — Durable Human Alert Acknowledgement

Step 46A closes the dashboard acknowledgement gap without turning Alerts & Audit into a trading/control dependency.

- `POST /api/alerts-audit/acknowledge` requires authenticated `OPERATOR` + CSRF.
- Requests bind to the exact durable watchdog lifecycle event (`acknowledgementKey`).
- A material alert update produces a new lifecycle key and requires a fresh acknowledgement.
- ACK is append-only and idempotent in the dedicated `dashboard-alert-ack-data` volume.
- ACK never means RESOLVED and never suppresses unresolved CRITICAL/WARN readiness counts.
- The acknowledgement store is separate from the Step 43 watchdog store and Step 46 manual-intent store.
- No NATS publish, trading PostgreSQL write, exchange API, signing, order submit/cancel or capital movement is introduced.
- Step 46B remains the next step: Independent Alert Notifier.
