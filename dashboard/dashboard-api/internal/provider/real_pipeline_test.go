package provider

import (
	"context"
	"errors"
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

type absentCheckpointRunner struct{ failure error }

func (runner absentCheckpointRunner) QueryJSON(context.Context, string, string) ([]byte, error) {
	return nil, runner.failure
}

func TestMissingDecisionIsPendingButDatabaseFailureRemainsAnError(t *testing.T) {
	p := &Real{postgres: pgstore.NewReader(pgstore.Config{DSN: "host=fixture dbname=fixture", Runner: absentCheckpointRunner{}})}
	pipeline, err := p.pipeline(context.Background())
	if err != nil || pipeline.EvidenceState != "PENDING" || pipeline.EmptyReason == "" || pipeline.Rows == nil {
		t.Fatalf("missing decision was not explained: %v %+v", err, pipeline)
	}
	risk, err := p.risk(context.Background())
	if err != nil || risk.EvidenceKind != "PENDING" || risk.AccountCashLabel != "Unavailable" {
		t.Fatalf("missing risk was interpreted as zero: %v %+v", err, risk)
	}
	p.postgres = pgstore.NewReader(pgstore.Config{DSN: "host=fixture dbname=fixture", Runner: absentCheckpointRunner{failure: errors.New("fixture database unreachable")}})
	if _, err = p.pipeline(context.Background()); err == nil {
		t.Fatal("database failure concealed as quiet cycle")
	}
	if _, err = p.risk(context.Background()); err == nil {
		t.Fatal("database failure concealed as unavailable decision")
	}
}

func TestBuildRealPipelineUsesDurableApprovedNotionalAndDoesNotInventRSI(t *testing.T) {
	checkpoint := pgstore.PipelineCheckpoint{
		Timestamp: 20260919,
		Signals:   tradingwire.StrategyIntentBatch{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "sig", CorrelationID: "corr"}, Timestamp: 20260919, Strategies: []tradingwire.StrategySignalIntent{{StrategyID: 1, StrategyName: "PureRSI", Signals: map[string]float64{"BTC": 1}}}},
		Account:   tradingwire.AccountSnapshot{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "acct"}, Timestamp: 20260919, Cash: 100000, Positions: map[string]float64{}},
		Decision:  tradingwire.DecisionBatch{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "dec", CorrelationID: "corr"}, DecisionTimestamp: 20260919, Strategies: []tradingwire.StrategyDecisionIntent{{StrategyID: 1, DecisionTimestamp: 20260919, ReferenceCapital: 100000, TargetNotionalUSD: map[string]float64{"BTC": 10000}}}},
	}
	update := tradingwire.MarketDataUpdated{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "m"}, CompletedThrough: 20260919, Source: "binance", Timeframe: "1d", ActiveTopN: 20}
	checkpoint.StrategyUpdate = &update
	request := tradingwire.NotionalOrderPlanningRequest{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "req"}, DecisionTimestamp: 20260919, ReferenceCloses: tradingwire.DailyCloseSnapshot{Date: 20260919, Closes: map[string]float64{"BTC": 50000}}, State: tradingwire.ExecutionPlanningState{StateRevision: 7, StrategyPositions: []tradingwire.PlanningStrategyPositions{{StrategyID: 1, Positions: map[string]float64{"BTC": 0.1}}}}}
	checkpoint.PlanningRequest = &request
	plan := tradingwire.NotionalOrderPlanBatch{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "plan"}, DecisionTimestamp: 20260919, StateRevision: 7, GlobalTargetNotionalUSD: map[string]float64{"BTC": 10000}, SubmitOrders: []tradingwire.PlannedNotionalOrder{{OrderID: 9, StrategyID: 1, Coin: "BTC", Side: 0, DeltaNotionalUSD: 5000, NotionalUSD: 5000}}}
	checkpoint.Plan = &plan
	state := pgstore.RuntimeState{UpdatedAt: "2026-09-20T11:15:06Z", Snapshot: tradingwire.TradingStateSnapshot{SchemaVersion: 1, Orders: []tradingwire.PersistedTrackedOrder{{OrderID: 9, StrategyID: 1, Coin: "BTC", Side: 0, Quantity: 0.1, Status: 4, FilledQuantity: 0.1}}}}
	data := buildRealPipeline(checkpoint, state, realReconciliationData{}, nil, "LIVE")
	if len(data.Rows) != 1 {
		t.Fatalf("expected one row, got %+v", data.Rows)
	}
	row := data.Rows[0]
	if row.RSILabel != "—" || row.TargetLabel != "10,000$" || row.ApprovedTargetLabel != "—" || row.ExchangeState != "FILLED" {
		t.Fatalf("unexpected row: %+v", row)
	}
	trace := data.Traces[row.CycleID]
	if trace.Strategy.RSI != "Not persisted" || trace.Portfolio.ApprovedTarget != "$10000.00" || trace.Planning.StateRevision != "7" {
		t.Fatalf("unexpected trace: %+v", trace)
	}

	// The same-cycle risk evaluation fills the sized/approved weights and the decision.
	sized, approved, held := 0.10, 0.08, 0.1
	checkpoint.RiskDiagnostics = &pgstore.RiskDiagnostics{Strategies: []pgstore.RiskStrategyEvaluation{{StrategyID: 1,
		Assets: map[string]pgstore.RiskAssetEvaluation{"BTC": {SizedWeight: &sized, ApprovedWeight: &approved, CurrentQuantity: &held, Action: "TARGET_WEIGHT"}}}}}
	row = buildRealPipeline(checkpoint, state, realReconciliationData{}, nil, "LIVE").Rows[0]
	if row.RawTargetLabel != "10.0%" || row.ApprovedTargetLabel != "8.0%" || row.DecisionLabel != "RESIZE" {
		t.Fatalf("risk evaluation not applied: %+v", row)
	}
	if row.SignalValue == nil || *row.SignalValue != 1 || len(row.Strategies) != 1 || row.Strategies[0].SignalLabel != "+1.00" ||
		row.Strategies[0].ApprovedLabel != "8.0%" || row.Strategies[0].Decision != "RESIZE" {
		t.Fatalf("strategy breakdown: %+v", row.Strategies)
	}
}

func TestPipelineQuietCycleShowsReferenceAssetsWithoutInventingTargets(t *testing.T) {
	checkpoint := pgstore.PipelineCheckpoint{Timestamp: 20261007,
		Account: tradingwire.AccountSnapshot{Positions: map[string]float64{"ETH": -2}},
		Signals: tradingwire.StrategyIntentBatch{Strategies: []tradingwire.StrategySignalIntent{{StrategyID: 1, Signals: map[string]float64{}}}}}
	request := tradingwire.NotionalOrderPlanningRequest{DecisionTimestamp: 20261007, ReferenceCloses: tradingwire.DailyCloseSnapshot{Date: 20261007, Closes: map[string]float64{"BTC": 60000, "ETH": 2000}}}
	checkpoint.PlanningRequest = &request
	plan := tradingwire.NotionalOrderPlanBatch{DecisionTimestamp: 20261007}
	checkpoint.Plan = &plan
	data := buildRealPipeline(checkpoint, pgstore.RuntimeState{}, realReconciliationData{}, nil, "PAPER")
	if len(data.Rows) != 2 || data.EvidenceState != "OBSERVED" {
		t.Fatalf("quiet cycle disappeared: %+v", data)
	}
	for _, row := range data.Rows {
		if row.Signal != "UNKNOWN" || row.TargetLabel != "—" || row.DecisionLabel != "" || row.PlannedAction != "NO ORDER" {
			t.Fatalf("invented quiet-cycle evidence: %+v", row)
		}
	}
	checkpoint.Plan = nil
	data = buildRealPipeline(checkpoint, pgstore.RuntimeState{}, realReconciliationData{}, nil, "PAPER")
	if data.EvidenceState != "PENDING" || data.Rows[0].ExchangeState != "PENDING" || data.Rows[0].PlannedAction != "Planner pending" {
		t.Fatalf("missing planner became no-action success: %+v", data)
	}
}
