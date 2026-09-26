# Step 29 — Resilience / performance hardening

Step 29 hardens the dashboard itself without changing any trading business path.

## Backend safeguards

- Every provider read is bounded by `DASHBOARD_RESOURCE_TIMEOUT` (default `7s`).
- SSE clients are bounded by `DASHBOARD_SSE_MAX_CLIENTS` (default `16`).
- Excess SSE connections fail with HTTP 503 + `Retry-After`; they never consume unbounded goroutines/connections.
- SSE advertises a 3-second reconnect delay and preserves the Step 28 heartbeat/fallback-refresh behaviour.
- Graceful container stop window is 10 seconds.
- HTTP request logs now include status and duration; 5xx or >=2s requests are logged at WARN.
- The API keeps bounded in-process operational counters only; no trading state is persisted or modified.

## Diagnostics

Authenticated endpoint: `GET /api/diagnostics`.

It exposes dashboard-process-only health:

- API version / uptime.
- request count, current/peak concurrency, 5xx rate and mean REST latency.
- resource timeout count and configured timeout.
- active/max/rejected SSE clients.
- Go goroutine count and memory footprint.

The Infrastructure page displays these metrics in a `Dashboard API Resilience` panel. These are observability metrics for the dashboard process, not trading readiness evidence.

## Frontend burst control

NATS invalidations are already coalesced server-side. Step 29 also coalesces each mounted React resource for 200 ms so a burst does not cause redundant REST requests.

## Safety boundary unchanged

- Browser never connects to NATS.
- SSE carries invalidations, not raw trading payloads.
- Dashboard API remains read-only for trading data except the existing fail-closed manual-control preview.
- No exchange order/control path is added.
- Dashboard failure must not stop trading.
