# Runtime contract audit — uploaded project snapshot

This is a concise evidence index for the Step 14 source audit. Paths are relative to the uploaded project root.

## Transport identity

- `lib/src/contracts/contract_metadata.h`
  - `schema_version`
  - `message_id` is producer-owned idempotency key
  - `correlation_id` links one workflow
  - `produced_at`
- `lib/src/contracts/live_execution_identity.h`
  - deterministic identities for live execution planning boundaries

## Canonical subjects

- `lib/src/transport/transport_subjects.h`
- Serialization: `lib/src/transport/contract_json_codec.cpp`

Dashboard integration should consume the existing JSON wire format, not invent a parallel serialization schema.

## Market data

- Writer/schema: `live_trading/market_data_service/src/market_data_store.cpp`
- Read-only canonical access: `lib/src/market/canonical_market_data_reader.cpp`
- Update publisher: `live_trading/market_data_service/src/market_data_update_publisher.cpp`
- Contract: `lib/src/contracts/market_data_updated.h`

Durability ordering is explicit: SQLite commit occurs before `market.data.updated.v1` publication.

## Strategy

- Service: `live_trading/strategy_service/src/strategy_service_main.cpp`
- Contract: `lib/src/contracts/strategy_intent_batch.h`
- Engine: `lib/src/runtime/strategy_signal_engine.cpp`
- Strategy config: `config/strategies/pure_rsi_signal.json`

The durable strategy checkpoint stores both the triggering MarketDataUpdated payload and resulting StrategyIntentBatch.

## PortfolioRisk

- Service: `live_trading/portfolio_risk_service/src/portfolio_risk_service_main.cpp`
- Engine: `lib/src/runtime/portfolio_risk_engine.cpp`
- Contract: `lib/src/contracts/decision_batch.h`
- Constraints: `lib/src/risk/risk_constraints.h`
- Portfolio config: `config/portfolio/*.json`

Important audit conclusion: `DecisionBatch` is the **approved** boundary. It does not preserve the full pre-constraint sizing diagnostic chain required by the final Why UI.

## Order planning

- Service: `live_trading/order_planner_service/src/order_planner_service_main.cpp`
- Contracts: `lib/src/contracts/notional_order_planning.h`
- Planner checkpoint table: `order_planner_live_notional_checkpoint`

The notional plan persists reference closes and per-order target/current/pending/delta notionals plus `state_revision`, making it a strong read source.

## Execution state

- Service: `live_trading/execution_state_service/src/execution_state_service_main.cpp`
- State store: `lib/src/persistence/postgres_state_store.cpp`
- Snapshot model: `lib/src/persistence/trading_state_snapshot.h`
- Account contract: `lib/src/contracts/account_snapshot.h`

ExecutionState owns durable account/order operational state. Fills are append-only in PostgreSQL.

## Reconciliation

- Snapshot contract: `lib/src/contracts/exchange_snapshot_event.h`
- Normalized snapshot model: `lib/src/recovery/exchange_snapshot.h`
- Reconciler: `lib/src/recovery/reconciler.cpp`
- Report: `lib/src/recovery/reconciliation_report.h`

The comparison exists, but the resulting report/clean flag is not a durable dashboard read model.

## Exchange edge

- Service: `live_trading/exchange_gateway/src/exchange_gateway_main.cpp`
- Live compose: `deploy/live/docker-compose.yml`

The exchange gateway is currently profile-gated (`exchange-edge`) in the live pre-exchange topology. Do not represent venue connectivity as production-active by default.

## Current strategy/config facts verified

From `config/strategies/pure_rsi_signal.json`:

- strategy id: 1
- type: `PureRSI`
- max active signals: 10
- universe: top 20 liquidity, SMA Volume(25), descending
- ranker: RSI Close(7), descending
- RSI entry: 80
- RSI exit: 70

From portfolio configs:

- max gross leverage: 1.50
- max asset weight: 1.50
- equal-weight mode uses 10% per full signal
- VolTarget mode uses target volatility 0.20 and 5-period covariance lookback

These values belong to configuration, not hard-coded dashboard behavior. The future API must expose active config/version rather than compile these values into React.
