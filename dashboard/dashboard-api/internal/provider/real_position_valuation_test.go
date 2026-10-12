package provider

import (
	"math"
	"testing"

	"control-dashboard-api/internal/tradingwire"
)

func TestAverageEntryPricesAddsReducesAndFlips(t *testing.T) {
	fills := []tradingwire.Fill{
		{FillID: 1, Timestamp: 1, Coin: "BTCUSDT", Side: 0, Quantity: 1, Price: 100},
		{FillID: 2, Timestamp: 2, Coin: "BTCUSDT", Side: 0, Quantity: 1, Price: 200}, // add: average 150
		{FillID: 3, Timestamp: 3, Coin: "BTCUSDT", Side: 1, Quantity: 1, Price: 300}, // reduce: keeps 150
		{FillID: 4, Timestamp: 4, Coin: "ETHUSDT", Side: 0, Quantity: 2, Price: 10},
		{FillID: 5, Timestamp: 5, Coin: "ETHUSDT", Side: 1, Quantity: 3, Price: 12}, // flip short at 12
		{FillID: 6, Timestamp: 6, Coin: "SOLUSDT", Side: 0, Quantity: 1, Price: 5},
		{FillID: 7, Timestamp: 7, Coin: "SOLUSDT", Side: 1, Quantity: 1, Price: 6}, // flat: no entry
	}
	entries := averageEntryPrices(fills)
	if entries["BTCUSDT"] != 150 || entries["ETHUSDT"] != 12 || entries["SOLUSDT"] != 0 {
		t.Fatalf("entries = %v", entries)
	}
}

func TestApplyPositionValuationMarksWeightsTargetsAndPnL(t *testing.T) {
	data := realPositionsData{Positions: []realPosition{
		{Asset: "BTCUSDT", Side: "Long", Quantity: 2, EffectiveQty: 2},
		{Asset: "ETHUSDT", Side: "Short", Quantity: 10, EffectiveQty: -10},
	}}
	applyPositionValuation(&data, positionValuationInputs{
		cash:    1000,
		marks:   map[string]float64{"BTCUSDT": 100, "ETHUSDT": 10},
		targets: map[string]float64{"BTCUSDT": 250},
		entries: map[string]float64{"BTCUSDT": 90, "ETHUSDT": 12},
		status:  "ALIGNED",
	})
	// Equity = 1000 + 2*100 - 10*10 = 1100.
	if data.ValuationState != "AVAILABLE" || data.equityValue != 1100 || data.grossValue != 300 {
		t.Fatalf("valuation = %s equity %v gross %v", data.ValuationState, data.equityValue, data.grossValue)
	}
	btc, eth := data.Positions[0], data.Positions[1]
	if math.Abs(btc.CurrentWeightPct-200.0/1100*100) > 1e-9 || math.Abs(btc.TargetWeightPct-250.0/1100*100) > 1e-9 || btc.TargetQty != 2.5 {
		t.Fatalf("btc weights = %v / %v qty %v", btc.CurrentWeightPct, btc.TargetWeightPct, btc.TargetQty)
	}
	if btc.PnLUSD != 20 || eth.PnLUSD != 20 || !eth.TargetAvailable || eth.TargetWeightPct != 0 {
		t.Fatalf("pnl btc %v eth %v target %v", btc.PnLUSD, eth.PnLUSD, eth.TargetWeightPct)
	}
	if btc.Status != "ALIGNED" || data.UnrealizedPnL != "+$40.00" {
		t.Fatalf("status %s unrealized %s", btc.Status, data.UnrealizedPnL)
	}
}

func TestApplyPositionValuationWithoutMarksStaysUnavailable(t *testing.T) {
	data := realPositionsData{TotalEquity: "Not valued", Positions: []realPosition{{Asset: "BTCUSDT", Quantity: 1, EffectiveQty: 1}}}
	applyPositionValuation(&data, positionValuationInputs{cash: 1000, marks: map[string]float64{}, status: "PENDING"})
	if data.ValuationState == "AVAILABLE" || data.TotalEquity != "Not valued" || data.Positions[0].ValuationAvailable {
		t.Fatalf("unexpected valuation: %+v", data)
	}
}
