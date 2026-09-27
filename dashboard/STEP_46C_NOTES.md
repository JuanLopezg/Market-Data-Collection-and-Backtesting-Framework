# Step 46C — Telegram Notification Adapter

Step 46C adds Telegram as an **optional delivery adapter** behind the independent Step 46B notifier. The notifier remains observability-only and is not part of the trading dependency chain.

## Safe default and activation

- `DASHBOARD_NOTIFIER_SINK=TEST_FILE` remains the default.
- `DASHBOARD_NOTIFIER_SINK=TELEGRAM` is opt-in through deployment configuration.
- A real bot token/chat id is **not required to validate Step 46C**. The adapter is tested against a local HTTP mock and the runtime remains on `TEST_FILE` unless the operator explicitly enables Telegram.
- Real bot credentials must never be committed, pasted into source, built into an image, or sent through the dashboard/browser.

Supported secret inputs:

- `DASHBOARD_TELEGRAM_BOT_TOKEN` or `DASHBOARD_TELEGRAM_BOT_TOKEN_FILE`;
- `DASHBOARD_TELEGRAM_CHAT_ID` or `DASHBOARD_TELEGRAM_CHAT_ID_FILE`.

Configuring both the value and file variant for the same secret is rejected as ambiguous. Production should prefer mounted secret files when the deployment platform provides them.

## Telegram API boundary

The adapter uses the official HTTPS Bot API endpoint only:

- base: `https://api.telegram.org`;
- method: `sendMessage`;
- POST JSON;
- plain text only (no `parse_mode`);
- link previews disabled;
- message body bounded to 4000 runes, below Telegram's documented 4096-character `sendMessage` limit;
- request timeout is bounded (`8s` default, `1s..1m` accepted);
- response body is bounded to 1 MiB;
- redirects are not followed by the production HTTP client.

The bot token is part of the Telegram request URL, so raw `net/http` transport errors are deliberately **not** returned to logs/status because those errors can contain the full URL. API descriptions are redacted for both token and chat id before they can reach logs.

## Sink-specific bootstrap

Step 46B already has a durable `TEST_FILE` decision store. Reusing that exact store for Telegram would mean that old lifecycle events were already considered processed and currently active alerts would not receive an initial Telegram notification.

Therefore the default durable store is sink-specific:

- `TEST_FILE`: `/data/notifier` (the existing Step 46B store);
- `TELEGRAM`: `/data/notifier/telegram`.

The normal Step 46B bootstrap semantics then apply independently to Telegram: the complete bounded lifecycle is replayed, obsolete/resolved history is suppressed, and only the latest lifecycle event for alerts that are still active is eligible for initial delivery. This avoids flooding Telegram with historical events when the adapter is first enabled.

## Durable Telegram receipts

Successful Telegram responses are recorded append-only in:

`/data/notifier/telegram/telegram-receipts.jsonl`

Receipt contract: `step46c-telegram-receipt-v1`.

A receipt binds:

- deterministic `notificationId` (`notify:<sourceEventId>`),
- source lifecycle event id,
- local sent timestamp,
- Telegram `message_id` returned by `sendMessage`.

If a successful receipt already exists, a replay/restart does not submit that notification to Telegram again.

### External exactly-once limitation

The Telegram Bot API does not expose a caller-supplied idempotency key for `sendMessage`. The local durable receipt prevents duplicates after a success response has been observed and persisted, but no client can prove exactly-once delivery if Telegram accepted a message and the process/network failed before the response/receipt became durable. The deterministic `notificationId` is included in the plain-text message for correlation. The notifier never treats a failed/ambiguous request as a successful durable decision.

## Network isolation

Step 46C replaces the Step 46B `network_mode: none` runtime boundary with a dedicated Docker bridge used only by the notifier:

`dashboard-notifier-egress`

The notifier is **not** attached to `dashboard-private` or `algotrading-live`, receives no PostgreSQL/NATS/exchange/private-key configuration, and still mounts the Step 43 alert lifecycle read-only. The only implemented network adapter is Telegram delivery.

## What Step 46C still does not do

- no alert acknowledgement mutation;
- no alert resolution mutation;
- no NATS publish;
- no trading PostgreSQL writes;
- no order submit/cancel/modify;
- no private venue authentication/signing;
- no wallet/private key handling;
- no manual route enablement;
- no `tradingReady` / `liveReady` enablement.

## Validation

Run after rebuilding the real dashboard stack:

```bash
./scripts/step46c-telegram-adapter.sh
```

Expected final banner:

`STEP 46C: PASS — TELEGRAM NOTIFICATION ADAPTER VALIDATED`

A real Telegram message is intentionally not required by the authoritative Step 46C gate. If Telegram is later enabled with real deployment secrets, the same notifier starts from its sink-specific durable bootstrap and uses the dedicated egress-only network.

## Next step

After the runtime gate is confirmed PASS, the next step is **Step 47 — Hyperliquid API Surface & Venue Semantics Specification**. No credentials, signing, wallet or order submission are needed for Step 47.
