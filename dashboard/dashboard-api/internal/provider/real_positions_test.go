package provider

import (
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestBuildRealPositionsUsesDurableQuantitiesWithoutInventingValuation(t *testing.T) {
	state := pgstore.RuntimeState{
		UpdatedAt: "2026-09-24T17:00:00.000Z",
		Snapshot: tradingwire.TradingStateSnapshot{
			SchemaVersion:    1,
			AccountCash:      100000,
			AccountPositions: map[string]float64{"BTC": 0.5, "ETH": -2, "ZERO": 0},
			Strategies: []tradingwire.StrategyStateSnapshot{
				{StrategyID: 1, VirtualPositions: map[string]float64{"BTC": 0.5, "ETH": -1.25}},
				{StrategyID: 2, VirtualPositions: map[string]float64{"ETH": -0.75}},
			},
		},
	}

	data := buildRealPositions(state)
	if data.SourceMode != "REAL" || data.ActivePositions != 2 || data.AccountCash != "$100000.00" {
		t.Fatalf("unexpected positions summary: %+v", data)
	}
	if data.ValuationState != "UNAVAILABLE" || data.TotalEquity != "Not valued" || data.UnrealizedPnL != "Not available" {
		t.Fatalf("valuation must remain explicitly unavailable: %+v", data)
	}
	if len(data.Positions) != 2 || data.Positions[0].Asset != "BTC" || data.Positions[1].Asset != "ETH" {
		t.Fatalf("positions must be sorted and omit zero quantities: %+v", data.Positions)
	}
	eth := data.Positions[1]
	if eth.Side != "Short" || eth.Quantity != 2 || eth.LocalQty != -2 || eth.Status != "UNKNOWN" {
		t.Fatalf("unexpected ETH projection: %+v", eth)
	}
	if eth.ValuationAvailable || eth.TargetAvailable || eth.PnLAvailable || eth.ReconciliationAvailable {
		t.Fatalf("unwired facts must not be marked available: %+v", eth)
	}
	if len(eth.StrategyBreakdown) != 2 || eth.StrategyBreakdown[0].Quantity != -1.25 || eth.StrategyBreakdown[1].Quantity != -0.75 {
		t.Fatalf("unexpected strategy breakdown: %+v", eth.StrategyBreakdown)
	}
}
