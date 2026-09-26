# Step 14 code-review checklist — COMPLETE FOR CURRENT SNAPSHOT

The full sanitized project ZIP supplied on 2026-09-24 provided enough material to complete the initial backend mapping. No further code is required before starting Step 15.

## Verified from the uploaded project

- repository topology
- live/historical Docker Compose
- `lib/src/contracts/`
- canonical NATS subjects
- JSON codecs
- PostgreSQL runtime/checkpoint schema created in code
- canonical SQLite market-data schema/read path
- MarketData / Strategy / PortfolioRisk / OrderPlanner / ExecutionState / ExchangeGateway entrypoints
- current configuration files
- reconciliation model

## Later code requests may be narrower

If Step 15+ exposes an ambiguity, request only the relevant implementation or runtime evidence. Do not ask for or copy secrets.

## Never include in future handoffs

- exchange API keys
- AWS credentials
- database passwords
- private keys/certificates
- production cookies/tokens
- unredacted `.env` secret values
