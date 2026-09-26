package provider

import (
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func proofCheckpoint() pgstore.PipelineCheckpoint {
	checkpoint := pgstore.PipelineCheckpoint{
		Timestamp: 20260919,
		Signals: tradingwire.StrategyIntentBatch{
			Metadata:   tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "sig-1", CorrelationID: "corr-1"},
			Timestamp:  20260919,
			Strategies: []tradingwire.StrategySignalIntent{{StrategyID: 1, StrategyName: "PureRSI", Signals: map[string]float64{"BTC": 1}}},
		},
		Account: tradingwire.AccountSnapshot{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "acct-1"}, Timestamp: 20260919, Cash: 100000, Positions: map[string]float64{}},
		Decision: tradingwire.DecisionBatch{
			Metadata:          tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "dec-1", CorrelationID: "corr-1"},
			DecisionTimestamp: 20260919,
			Strategies:        []tradingwire.StrategyDecisionIntent{{StrategyID: 1, DecisionTimestamp: 20260919, ReferenceCapital: 100000, TargetNotionalUSD: map[string]float64{"BTC": 5000}}},
		},
	}
	update := tradingwire.MarketDataUpdated{Metadata: tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "market-1"}, CompletedThrough: 20260919, Source: "canonical-sqlite", Timeframe: "1d", ActiveTopN: 20}
	checkpoint.StrategyUpdate = &update
	request := tradingwire.NotionalOrderPlanningRequest{
		Metadata:          tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "request-1"},
		DecisionTimestamp: 20260919,
		ReferenceCloses:   tradingwire.DailyCloseSnapshot{Date: 20260919, Closes: map[string]float64{"BTC": 50000}},
		State:             tradingwire.ExecutionPlanningState{StateRevision: 7, StrategyPositions: []tradingwire.PlanningStrategyPositions{{StrategyID: 1, Positions: map[string]float64{}}}},
	}
	checkpoint.PlanningRequest = &request
	plan := tradingwire.NotionalOrderPlanBatch{
		Metadata:                tradingwire.ContractMetadata{SchemaVersion: 1, MessageID: "plan-1"},
		DecisionTimestamp:       20260919,
		StateRevision:           7,
		GlobalTargetNotionalUSD: map[string]float64{"BTC": 5000},
		SubmitOrders:            []tradingwire.PlannedNotionalOrder{{OrderID: 9, StrategyID: 1, Coin: "BTC", Side: 0, NotionalUSD: 5000}},
	}
	checkpoint.Plan = &plan
	return checkpoint
}

func TestEndToEndProofAlignedOnlyWithCompleteDurableChain(t *testing.T) {
	checkpoint := proofCheckpoint()
	state := pgstore.RuntimeState{UpdatedAt: "2026-09-20T11:15:06Z", Snapshot: tradingwire.TradingStateSnapshot{SchemaVersion: 1, Orders: []tradingwire.PersistedTrackedOrder{{OrderID: 9, StrategyID: 1, Coin: "BTC", Side: 0, Quantity: 0.1, Status: 4, FilledQuantity: 0.1}}}}
	recon := realReconciliationData{Status: "CLEAN", ComparisonAvailable: true, EvidenceFresh: true}
	fills := pgstore.FillWindow{Rows: []tradingwire.Fill{{FillID: 77, OrderID: 9, StrategyID: 1, Coin: "BTC", Quantity: 0.1, Price: 50000}}}

	proof := buildRealEndToEndProof(checkpoint, state, recon, nil, fills, nil, "LIVE")
	if proof.Status != "ALIGNED" || proof.Completion != "FULL_CHAIN" {
		t.Fatalf("unexpected proof verdict: %+v", proof)
	}
	if proof.MatchedOrders != 1 || proof.MatchedFills != 1 || len(proof.Steps) != 8 {
		t.Fatalf("unexpected evidence counts: %+v", proof)
	}
}

func TestEndToEndProofNoActionStaysPending(t *testing.T) {
	checkpoint := proofCheckpoint()
	checkpoint.Plan.SubmitOrders = nil
	state := pgstore.RuntimeState{UpdatedAt: "2026-09-20T11:15:06Z", Snapshot: tradingwire.TradingStateSnapshot{SchemaVersion: 1}}
	recon := realReconciliationData{Status: "CLEAN", ComparisonAvailable: true, EvidenceFresh: true}

	proof := buildRealEndToEndProof(checkpoint, state, recon, nil, pgstore.FillWindow{}, nil, "LIVE")
	if proof.Status != "PENDING" || proof.Completion != "NO_ACTION" {
		t.Fatalf("no-action cycle must not be called a full proof: %+v", proof)
	}
}

func TestEndToEndProofRejectedOrderIsBlocked(t *testing.T) {
	checkpoint := proofCheckpoint()
	state := pgstore.RuntimeState{UpdatedAt: "2026-09-20T11:15:06Z", Snapshot: tradingwire.TradingStateSnapshot{SchemaVersion: 1, Orders: []tradingwire.PersistedTrackedOrder{{OrderID: 9, StrategyID: 1, Coin: "BTC", Side: 0, Quantity: 0.1, Status: 6, LastMessage: "rejected"}}}}
	recon := realReconciliationData{Status: "CLEAN", ComparisonAvailable: true, EvidenceFresh: true}

	proof := buildRealEndToEndProof(checkpoint, state, recon, nil, pgstore.FillWindow{}, nil, "LIVE")
	if proof.Status != "BLOCKED" || proof.Completion != "CONTRADICTION_OR_TERMINAL_FAILURE" {
		t.Fatalf("rejected submit must block full-chain proof: %+v", proof)
	}
}
