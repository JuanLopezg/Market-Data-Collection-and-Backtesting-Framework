package postgres

// Table names verified against the uploaded C++ project snapshot. These are
// runtime-owned tables, not a license for browser-driven SQL. Dashboard queries
// remain bounded and server-side.
const (
	TableTradingRuntimeState                 = "trading_runtime_state"
	TableTradingFills                        = "trading_fills"
	TableStrategyServiceMetadata             = "strategy_service_metadata"
	TableStrategyMarketUpdateCheckpoint      = "strategy_market_update_checkpoint"
	TablePortfolioRiskServiceMetadata        = "portfolio_risk_service_metadata"
	TablePortfolioRiskLiveAccountCheckpoint  = "portfolio_risk_live_account_checkpoint"
	TablePortfolioRiskLiveDecisionCheckpoint = "portfolio_risk_live_decision_checkpoint"
	TableOrderPlannerLiveNotionalCheckpoint  = "order_planner_live_notional_checkpoint"
)

// Keys inside trading_runtime_state.snapshot JSONB, verified against
// lib/src/persistence/postgres_state_store.cpp.
var TradingRuntimeSnapshotKeys = []string{
	"schema_version",
	"last_bar_close_timestamp",
	"last_execution_timestamp",
	"next_order_id",
	"account_cash",
	"account_positions",
	"strategies",
	"pending_plans",
	"orders",
	"processed_fill_ids",
}
