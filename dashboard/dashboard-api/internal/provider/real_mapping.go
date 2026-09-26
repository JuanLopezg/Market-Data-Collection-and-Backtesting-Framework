package provider

type MappingStatus string

const (
	MappingEnabledRealRead MappingStatus = "ENABLED_REAL_READ"
	MappingVerifiedSource  MappingStatus = "VERIFIED_SOURCE"
	MappingNeedsProjection MappingStatus = "NEEDS_PROJECTION"
	MappingNotImplemented  MappingStatus = "NOT_IMPLEMENTED"
)

type ResourceMapping struct {
	Resource        Resource
	Owner           string
	CanonicalSource string
	Status          MappingStatus
	Detail          string
}

// Verified against the post-T24 C++ runtime during Step 14A. A verified source
// means ownership and canonical source are known; it does not mean the frontend
// resource is already safe to serve in real mode.
var RealResourceMappings = []ResourceMapping{
	{Resource: ResourceShellStatus, Owner: "aggregated", CanonicalSource: "PostgreSQL/NATS + canonical market data + durable runtime + retained reconciliation evidence + derived operational alerts", Status: MappingEnabledRealRead, Detail: "Step 27 serves a fail-closed global status aggregate; READY is intentionally withheld until exchange connectivity, common service liveness/trading state and host clock sync are independently observable"},
	{Resource: ResourceOverview, Owner: "aggregated", CanonicalSource: "ExecutionState + market valuation + future ledger", Status: MappingNeedsProjection, Detail: "cash/positions exist; equity/PnL history is not yet a canonical dashboard projection"},
	{Resource: ResourcePositions, Owner: "ExecutionState", CanonicalSource: "trading_runtime_state.snapshot", Status: MappingEnabledRealRead, Detail: "Step 17 serves durable physical quantities and strategy virtual positions; valuation/PnL/approved target/reconciliation remain explicitly unavailable"},
	{Resource: ResourceReconciliation, Owner: "ExecutionState", CanonicalSource: "trading_runtime_state.snapshot + latest retained execution.exchange.snapshot.v1", Status: MappingEnabledRealRead, Detail: "Step 19 performs the verified C++ Reconciler comparison read-only; stale/missing exchange evidence is PENDING rather than inferred current truth"},
	{Resource: ResourcePipeline, Owner: "Strategy/PortfolioRisk/OrderPlanner/ExecutionState", CanonicalSource: "cycle-aligned durable daily checkpoints + trading_runtime_state.snapshot + retained reconciliation evidence", Status: MappingEnabledRealRead, Detail: "Step 31 reconstructs the latest durable Strategy → Risk → Planner → Execution lineage and adds order/fill/reconciliation evidence proof without inferring non-persisted RSI/rank/intermediate risk transforms"},
	{Resource: ResourceExecution, Owner: "ExecutionState", CanonicalSource: "trading_runtime_state.snapshot.orders + trading_fills", Status: MappingEnabledRealRead, Detail: "Step 18 serves a bounded durable order/fill view; latency, slippage and replacement lineage remain explicitly unavailable where not persisted"},
	{Resource: ResourceRisk, Owner: "PortfolioRisk", CanonicalSource: "portfolio_risk_live_decision_checkpoint", Status: MappingEnabledRealRead, Detail: "Step 21 serves the exact approved DecisionBatch plus its persisted StrategyIntentBatch/AccountSnapshot inputs; active config limits, raw sizing, binding constraints, volatility transforms and breach reports remain explicitly unavailable"},
	{Resource: ResourceMarketData, Owner: "MarketData + Strategy", CanonicalSource: "canonical SQLite + strategy_market_update_checkpoint", Status: MappingEnabledRealRead, Detail: "Step 22 serves the canonical daily ranking/OHLCV window, recomputes SMA Volume(25)/RSI(7) with verified C++ formulas, and joins durable strategy signals only on exact timestamp alignment"},
	{Resource: ResourceInfrastructure, Owner: "operations", CanonicalSource: "PostgreSQL diagnostics + NATS monitoring + canonical market-data file probe", Status: MappingEnabledRealRead, Detail: "Step 16 serves real source health; host/container/exchange/reconciliation liveness remains UNKNOWN"},
	{Resource: ResourceAlertsAudit, Owner: "operations", CanonicalSource: "future append-only alert/audit read model", Status: MappingNeedsProjection, Detail: "structured logs exist but are not canonical durable alert state"},
	{Resource: ResourceLiveVsExpected, Owner: "analytics", CanonicalSource: "historical baseline + future live metric projections", Status: MappingNotImplemented, Detail: "do not derive statistical baselines ad hoc inside the API"},
	{Resource: ResourceManualControl, Owner: "dashboard trading-control preview", CanonicalSource: "current PortfolioRisk checkpoint + canonical market universe; future manual business contract must re-enter normal risk/planning/execution pipeline", Status: MappingNeedsProjection, Detail: "Step 26 enables authenticated server-side CSV validation and target-delta preview only; routing, manual risk approval and durable control audit remain fail-closed"},
}

func verifiedSourceMappingCount() int {
	count := 0
	for _, mapping := range RealResourceMappings {
		if mapping.Status == MappingVerifiedSource || mapping.Status == MappingEnabledRealRead {
			count++
		}
	}
	return count
}

func enabledRealReadCount() int {
	count := 0
	for _, mapping := range RealResourceMappings {
		if mapping.Status == MappingEnabledRealRead {
			count++
		}
	}
	return count
}
