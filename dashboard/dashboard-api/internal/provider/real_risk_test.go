package provider

import (
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestBuildRealRiskUsesApprovedDecisionBoundaryWithoutInventingLimits(t *testing.T) {
	checkpoint := pgstore.PipelineCheckpoint{
		Timestamp: 20260919,
		Signals: tradingwire.StrategyIntentBatch{
			Metadata:   tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "strategy-intents:20260919", CorrelationID: "market-data-updated:20260919"},
			Timestamp:  20260919,
			Strategies: []tradingwire.StrategySignalIntent{{StrategyID: 1, StrategyName: "Pure_RSI", Signals: map[string]float64{"BTCUSDT": 1, "ETHUSDT": 0}}},
		},
		Account: tradingwire.AccountSnapshot{
			Metadata:  tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "account-snapshot:20260919"},
			Timestamp: 20260919, Cash: 100000, Positions: map[string]float64{},
		},
		Decision: tradingwire.DecisionBatch{
			Metadata:          tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "portfolio-decision:20260919", CorrelationID: "market-data-updated:20260919"},
			DecisionTimestamp: 20260919,
			Strategies: []tradingwire.StrategyDecisionIntent{{
				StrategyID: 1, DecisionTimestamp: 20260919, ReferenceCapital: 100000,
				TargetNotionalUSD: map[string]float64{"BTCUSDT": 10000, "ETHUSDT": -5000},
				Decisions:         []tradingwire.RebalanceDecision{{Coin: "BTCUSDT", TargetWeight: 0.10}, {Coin: "ETHUSDT", TargetWeight: -0.05}},
			}},
		},
	}

	data := buildRealRisk(checkpoint)
	if data.SourceMode != "REAL" || data.ReferenceCapitalLabel != "$100000.00" {
		t.Fatalf("unexpected source/reference data: %+v", data)
	}
	if data.ApprovedGrossTargetLabel != "15.0%" || data.ApprovedNetTargetLabel != "+5.0%" || data.ApprovedTargetNotionalLabel != "$15000.00" {
		t.Fatalf("unexpected approved target metrics: %+v", data)
	}
	if data.ActiveLimitsAvailable || data.BreachesAvailable || data.CurrentValuationAvailable {
		t.Fatalf("step 21 must not claim unavailable diagnostics are real: %+v", data)
	}
	if len(data.Assets) != 2 || data.Assets[0].LimitLabel != "Active config not wired" {
		t.Fatalf("unexpected asset projection: %+v", data.Assets)
	}
}
