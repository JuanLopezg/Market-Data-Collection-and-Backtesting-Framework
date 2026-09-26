package catalog

import (
	"fmt"

	"control-dashboard-api/internal/provider"
)

type Status string

const (
	StatusVerifiedBase Status = "VERIFIED_BASE"
	StatusPartial      Status = "PARTIAL"
	StatusMissing      Status = "MISSING_PROJECTION"
	StatusNotBuilt     Status = "NOT_IMPLEMENTED"
)

type Mapping struct {
	Resource provider.Resource `json:"resource"`
	Status   Status            `json:"status"`
	Owner    string            `json:"owner"`
	Sources  []string          `json:"sources"`
	Gaps     []string          `json:"gaps,omitempty"`
}

func Mappings() []Mapping {
	return []Mapping{
		{
			Resource: provider.ResourceShellStatus,
			Status:   StatusPartial,
			Owner:    "dashboard aggregation over runtime-owned health/state",
			Sources: []string{
				"PostgreSQL runtime/checkpoint timestamps",
				"canonical SQLite market-data frontier",
				"NATS/JetStream server diagnostics",
			},
			Gaps: []string{
				"no project-wide service heartbeat/readiness contract",
				"execution-state reconciliation flag is in-memory only",
				"exchange-gateway is profile-gated in current live compose",
			},
		},
		{
			Resource: provider.ResourceOverview,
			Status:   StatusPartial,
			Owner:    "ExecutionState + market data + future accounting read model",
			Sources: []string{
				"trading_runtime_state.snapshot account_cash/account_positions",
				"canonical SQLite OHLCV closes",
				"trading_fills append-only fill history",
			},
			Gaps: []string{
				"no durable realized/unrealized PnL ledger",
				"no bounded equity-curve/Sharpe read model",
			},
		},
		{
			Resource: provider.ResourcePositions,
			Status:   StatusVerifiedBase,
			Owner:    "ExecutionState",
			Sources: []string{
				"trading_runtime_state.snapshot account_positions/strategies/orders",
				"portfolio_risk_live_decision_checkpoint latest approved target",
				"canonical SQLite close prices",
			},
			Gaps: []string{"weight/notional fields are dashboard derivations and must expose their timestamp"},
		},
		{
			Resource: provider.ResourceReconciliation,
			Status:   StatusPartial,
			Owner:    "ExecutionState reconciliation boundary + ExchangeGateway",
			Sources: []string{
				"execution.exchange.snapshot.v1 ExchangeSnapshotEvent",
				"trading_runtime_state.snapshot local positions/orders",
			},
			Gaps: []string{
				"latest exchange snapshot/reconciliation report is not durably persisted",
				"reconciled_ is local ExecutionState memory, not a dashboard-readable contract",
			},
		},
		{
			Resource: provider.ResourcePipeline,
			Status:   StatusPartial,
			Owner:    "Strategy -> PortfolioRisk -> OrderPlanner -> ExecutionState",
			Sources: []string{
				"strategy_market_update_checkpoint update_payload/intent_payload",
				"portfolio_risk_live_decision_checkpoint signals/account/decision payloads",
				"order_planner_live_notional_checkpoint request_payload/plan_payload",
				"trading_runtime_state.snapshot orders",
			},
			Gaps: []string{
				"live risk sizing/constraint diagnostics are not emitted in DecisionBatch",
				"RSI/rank values must be recomputed from canonical market data/config for Why trace",
			},
		},
		{
			Resource: provider.ResourceExecution,
			Status:   StatusVerifiedBase,
			Owner:    "ExecutionState + ExchangeGateway",
			Sources: []string{
				"trading_runtime_state.snapshot orders",
				"trading_fills",
				"order_planner_live_notional_checkpoint plan_payload for reference close/notional",
				"execution.event.order_update.v1 / execution.event.fill.v1",
			},
			Gaps: []string{"long historical order lifecycle is not append-only persisted separately from current snapshot"},
		},
		{
			Resource: provider.ResourceRisk,
			Status:   StatusPartial,
			Owner:    "PortfolioRisk",
			Sources: []string{
				"portfolio config JSON risk/sizer/rebalance settings",
				"portfolio_risk_live_decision_checkpoint",
				"trading_runtime_state.snapshot + canonical closes for current utilisation",
			},
			Gaps: []string{"pre/post constraint diagnostics and binding-rule evidence are not in the live DecisionBatch contract"},
		},
		{
			Resource: provider.ResourceMarketData,
			Status:   StatusVerifiedBase,
			Owner:    "MarketData + Strategy",
			Sources: []string{
				"SQLite ohlcv_data",
				"SQLite market_volume_rank_daily",
				"SQLite tracked_pairs",
				"market.data.updated.v1",
				"strategy_market_update_checkpoint / strategy.intents.v1",
			},
			Gaps: []string{"candidate-rejection reason is not persisted as a first-class contract"},
		},
		{
			Resource: provider.ResourceInfrastructure,
			Status:   StatusPartial,
			Owner:    "deployment/observability layer",
			Sources: []string{
				"PostgreSQL connection health",
				"NATS monitoring endpoint / JetStream diagnostics",
				"Docker deployment topology",
			},
			Gaps: []string{
				"no service health HTTP contract",
				"no safe host/container metrics adapter is implemented",
				"do not mount Docker socket into dashboard-api merely for convenience",
			},
		},
		{
			Resource: provider.ResourceAlertsAudit,
			Status:   StatusPartial,
			Owner:    "dashboard-derived operational projection + future durable alert/audit store",
			Sources: []string{
				"PostgreSQL/NATS source health",
				"reconciliation retained exchange evidence",
				"trading_runtime_state/trading_fills execution evidence",
				"canonical market-data diagnostics + PortfolioRisk checkpoint",
			},
			Gaps: []string{
				"no append-only alert history store",
				"no durable acknowledgement/resolution state",
				"no durable human-action audit table for login/control/config/deploy actions",
			},
		},
		{
			Resource: provider.ResourceLiveVsExpected,
			Status:   StatusMissing,
			Owner:    "future historical/live analytics projection",
			Sources:  []string{"research/backtest diagnostics exist but no canonical dashboard baseline store exists"},
			Gaps:     []string{"requires explicit baseline generation/versioning and bounded live metric projections"},
		},
		{
			Resource: provider.ResourceManualControl,
			Status:   StatusPartial,
			Owner:    "dashboard trading-control preview + future runtime control contract",
			Sources: []string{
				"authenticated OPERATOR session + CSRF",
				"portfolio_risk_live_decision_checkpoint reference target",
				"canonical SQLite market universe",
			},
			Gaps: []string{
				"no verified manual target command exists in the C++ business pipeline",
				"manual PortfolioRisk transformation/approval is not implemented",
				"durable human-action audit persistence is not implemented",
			},
		},
	}
}

func Validate() error {
	seen := make(map[provider.Resource]bool, len(provider.AllResources))
	for _, mapping := range Mappings() {
		if seen[mapping.Resource] {
			return fmt.Errorf("duplicate integration catalog resource %q", mapping.Resource)
		}
		seen[mapping.Resource] = true
	}
	for _, resource := range provider.AllResources {
		if !seen[resource] {
			return fmt.Errorf("integration catalog missing resource %q", resource)
		}
	}
	return nil
}
