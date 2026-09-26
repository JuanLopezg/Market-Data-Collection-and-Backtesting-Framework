# Step 28 — Authenticated SSE / read-only NATS invalidation bridge

Step 28 removes aggressive browser polling without making the browser a NATS client.

## Data path

1. `dashboard-api` opens a read-only NATS protocol subscription to audited runtime subjects.
2. A trading event is converted only into a dashboard **resource invalidation**.
3. `/api/stream` sends the invalidation to the authenticated browser over Server-Sent Events (SSE).
4. Only mounted React read models refetch their normal authenticated REST endpoint.
5. NATS payloads are never exposed directly to the browser.

The bridge never publishes, ACKs, creates JetStream consumers, changes durable state, or sends orders.

## Resilience

- SSE heartbeat: 15 seconds.
- Fail-safe mounted-view refresh: 30 seconds, so PostgreSQL/SQLite-only changes are eventually visible even if no matching NATS event was observed.
- NATS subscriber reconnect uses bounded exponential backoff.
- Burst invalidations are coalesced for 175 ms.
- A full subject channel drops excess invalidations rather than blocking NATS/trading; the 30-second fail-safe refresh recovers the UI.
- Existing dashboard REST read models remain authoritative.
- Logout/session expiry terminates the stream; authentication is rechecked during heartbeats.
- Caddy disables buffering/compression on `/api/stream`.

## UI

The global top bar now shows `SSE LIVE`, `CONNECTING`, or `RECONNECTING`.

## Still intentionally not done

- No browser -> NATS connection.
- No NATS publish/control path.
- No durable dashboard event store yet.
- No manual-control routing.
- No claim of global `READY` until the missing readiness evidence is implemented.
