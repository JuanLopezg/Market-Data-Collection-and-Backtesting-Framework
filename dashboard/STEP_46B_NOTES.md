# Step 46B — Independent Alert Notifier

Step 46B adds a notification process that is independent from both the trading brain and the web/API dashboard.

## Boundary

- New process/container: `dashboard-alert-notifier` / `control-dashboard-alert-notifier`.
- Reads only the Step 43 durable watchdog lifecycle volume (`/data/watchdog`) as **read-only**.
- Writes only its own observability volume (`/data/notifier`).
- In Step 46B the container has `network_mode: none`.
- It has no PostgreSQL DSN, NATS URL, venue endpoint, wallet, signing material or trading-control API.
- Trading and dashboard/API continue if the notifier is stopped or restarted.

## Durable notifier state

Store version: `step46b-v1`.

Files in the dedicated notifier volume:

- `events.jsonl`: append-only delivery/suppression decisions keyed by the exact watchdog lifecycle `eventId`.
- `status.json`: atomic heartbeat/status projection.
- `test-sink.jsonl`: Step 46B notification sink used to prove delivery without any external notification provider.

Every source lifecycle event is processed once. The notifier replays its own decision log on restart and refuses duplicate source-event decisions.

## Bootstrap / restart behaviour

The first start replays the complete bounded Step 43 lifecycle stream but does **not** flood historical resolved/obsolete alerts. Historical lifecycle events are recorded as suppressed with `BOOTSTRAP_HISTORY`; only the latest lifecycle instance of alerts that are still active is eligible for a bootstrap notification.

A `BOOTSTRAP_COMPLETE` marker is appended only after the initial replay is complete. If the process dies during bootstrap, the next start continues bootstrap semantics for unprocessed events rather than treating old history as new live alerts.

## Deduplication, cooldown and escalation

Default policy:

- minimum severity: `WARN`;
- `OPENED`: delivered when severity is at/above the configured minimum;
- `UPDATED`: same/lower severity is suppressed during the cooldown window;
- severity increase (for example `WARN -> CRITICAL`) bypasses cooldown;
- an update after cooldown is delivered;
- `RESOLVED` is delivered only if that alert previously produced a notification;
- every delivered notification uses deterministic id `notify:<sourceEventId>`.

The Step 46B `TEST_FILE` sink is idempotent by `notificationId`, so a crash between sink delivery and decision persistence does not duplicate the test notification after restart.

## What Step 46B does not do

- No Telegram/network delivery yet.
- No acknowledgement mutation.
- No alert resolution mutation.
- No NATS publish.
- No trading PostgreSQL writes.
- No order submit/cancel/modify.
- No private auth/signing/wallet handling.
- No readiness or routing enablement.

`DASHBOARD_NOTIFIER_SINK=TELEGRAM` is intentionally rejected in Step 46B. Telegram is Step 46C.

## Next step

After the runtime gate is confirmed PASS, the next step is **Step 46C — Telegram Notification Adapter**.
