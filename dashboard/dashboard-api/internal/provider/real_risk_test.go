package provider

import (
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestPersistedRiskEvaluationExplainsSignedCapsAndHold(t *testing.T) {
	available := true
	raw, capped, approved, quantity := -0.5, -0.25, -0.2, -2.0
	data := buildRealRisk(pgstore.PipelineCheckpoint{})
	report := &pgstore.RiskDiagnostics{ConfigurationFingerprint: "cycle identity digest", Strategies: []pgstore.RiskStrategyEvaluation{{
		StrategyID: 1, Name: "Fixture", Sizer: "EqualWeight", SizingAvailable: &available,
		ReferenceCapital: 980, MaxAssetWeight: 0.25, MaxGrossLeverage: 0.4, GrossAfterAssetCap: 0.5, GrossScale: 0.8,
		VolatilityState: "NOT_APPLICABLE", Assets: map[string]pgstore.RiskAssetEvaluation{"ETH": {SizedWeight: &raw, AssetCappedWeight: &capped, ApprovedWeight: &approved, CurrentQuantity: &quantity, Action: "HOLD", Reductions: []string{"ASSET_CAP", "GROSS_CAP"}}},
	}}}
	applyRiskEvaluations(&data, report)
	if len(data.RiskEvaluations) != 1 || data.RiskEvaluations[0].Assets[0].SizedWeight != "-50.0%" || data.RiskEvaluations[0].Assets[0].Action != "HOLD" || data.CurrentValuationAvailable || data.BreachesAvailable {
		t.Fatalf("Incorrect evaluation projection: %+v", data)
	}
	available = false
	report.Strategies[0].Assets = nil
	data = buildRealRisk(pgstore.PipelineCheckpoint{})
	applyRiskEvaluations(&data, report)
	if data.RiskEvaluations[0].State != "SIZING UNAVAILABLE / HOLD" || data.RiskEvaluations[0].GrossScale != "Not evaluated" {
		t.Fatal("Unavailable sizing shown as zero exposure")
	}
}

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

func TestRiskQuietCyclePreservesHoldAndShowsPersistedPolicy(t *testing.T) {
	checkpoint := pgstore.PipelineCheckpoint{Timestamp: 20261007,
		PortfolioConfig: `{"strategies":[{"id":1,"name":"Pure_RSI","allocation_weight":1,"sizer":{"type":"equal_weight","weight_per_full_signal":0.1},"risk":{"max_gross_leverage":1.5,"max_asset_weight":1.5},"rebalance":{"type":"entry_exit_only"}}]}` + "\nmarket_data_mode=canonical-sqlite-v1",
		Account:         tradingwire.AccountSnapshot{Cash: 100000, Positions: map[string]float64{"BTC": 2}},
		Signals:         tradingwire.StrategyIntentBatch{Strategies: []tradingwire.StrategySignalIntent{{StrategyID: 1, Signals: map[string]float64{"ETH": 0}}}}}
	data := buildRealRisk(checkpoint)
	if data.RiskState != "NO NEW TARGETS" || data.ReferenceCapitalLabel != "Not emitted" || data.StrategyCount != 1 || data.AccountCashLabel != "$100000.00" {
		t.Fatalf("quiet decision mislabeled: %+v", data)
	}
	if len(data.Assets) != 2 || data.Assets[0].Signal != "UNKNOWN" || data.Assets[1].Signal != "FLAT" || data.Assets[0].TargetNotionalLabel != "HOLD / no new target" {
		t.Fatalf("held position converted to flat/zero: %+v", data.Assets)
	}
	if !data.ActiveLimitsAvailable || data.BreachesAvailable || len(data.Policies) != 1 || data.Policies[0].GrossLimitLabel != "150.0%" {
		t.Fatalf("policy evidence lost/fabricated: %+v", data)
	}
	if len(checkpointRiskPolicies(`{"strategies":[{"id":1}]}`)) != 0 {
		t.Fatal("incomplete policy accepted as zero limits")
	}
}

func TestRiskAggregateWeightUsesAllocatedCapitalInsteadOfAddingStrategyPercentages(t *testing.T) {
	checkpoint := pgstore.PipelineCheckpoint{Decision: tradingwire.DecisionBatch{Strategies: []tradingwire.StrategyDecisionIntent{
		{StrategyID: 1, ReferenceCapital: 75000, TargetNotionalUSD: map[string]float64{"BTC": 7500}, Decisions: []tradingwire.RebalanceDecision{{Coin: "BTC", TargetWeight: .1}}},
		{StrategyID: 2, ReferenceCapital: 25000, TargetNotionalUSD: map[string]float64{"BTC": 2500}, Decisions: []tradingwire.RebalanceDecision{{Coin: "BTC", TargetWeight: .1}}}}}}
	data := buildRealRisk(checkpoint)
	if len(data.Assets) != 1 || data.Assets[0].ApprovedWeightLabel != "10.0%" {
		t.Fatalf("strategy percentages incorrectly added: %+v", data.Assets)
	}
}
