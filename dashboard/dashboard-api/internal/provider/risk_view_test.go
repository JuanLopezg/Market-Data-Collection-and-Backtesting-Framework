package provider

import (
	"math"
	"testing"
)

func TestRiskHistoryDrawdownAndLeverage(t *testing.T) {
	gross := 150.0
	history, worst := riskHistory([]riskEquityPoint{
		{label: "d1", equity: 100},
		{label: "d2", equity: 120, gross: &gross},
		{label: "d3", equity: 90},
		{label: "d4", equity: 0}, // not valued: skipped
		{label: "d5", equity: 108},
	})
	if len(history) != 4 || history[1].Leverage == nil || *history[1].Leverage != 1.25 || history[0].Leverage != nil {
		t.Fatalf("history = %+v", history)
	}
	if history[2].Drawdown != -25 || history[3].Drawdown != -10 || worst != -25 {
		t.Fatalf("drawdowns = %+v worst %v", history, worst)
	}
}

func TestRiskAccountViewCardsTonesAndCapUtilization(t *testing.T) {
	approved := 0.10
	cards, exposures := riskAccountView(1000, []riskPositionValue{
		{asset: "BTC", value: 1500, approved: &approved},
		{asset: "ETH", value: -500},
	}, 1.5, []riskHistoryPoint{{Label: "d1", Drawdown: -12}}, -30)
	// Gross 2000 / 1000 = 2.00x: orange band.
	if cards[0].Value != "2.00x" || cards[0].Tone != "elevated" || cards[2].Value != "100.0%" || cards[3].Value != "BTC 150.0%" {
		t.Fatalf("cards = %+v", cards)
	}
	if cards[4].Tone != "elevated" || cards[5].Tone != "negative" {
		t.Fatalf("drawdown tones = %+v", cards[4:])
	}
	if exposures[0].Asset != "BTC" || exposures[0].Approved != "10.0%" || math.Abs(exposures[0].Utilization-100) > 1e-9 || exposures[1].Side != "Short" {
		t.Fatalf("exposures = %+v", exposures)
	}
	if leverageTone(1.5) != "positive" || leverageTone(2.6) != "negative" {
		t.Fatal("leverage scale")
	}
}

func TestSimulationRiskViewUsesDecisionLimits(t *testing.T) {
	gross := 50000.0
	s := simulationSnapshot{
		Account:       simulationAccount{Equity: 100000, Positions: []simulationPosition{{Asset: "BTC", Quantity: 1, MarkPrice: 50000}}},
		EquityHistory: []simulationEquityPoint{{Label: "2024-01-01", Equity: 100000, Gross: &gross}},
		Pipeline: &simulationPipeline{Assets: []simulationPipelineAsset{{Asset: "BTC", ApprovedWeight: 0.5,
			Strategies: []simulationPipelineStrategy{{StrategyID: 1, Name: "Pure_RSI", Capital: 100000, Share: 1, Sizer: "EqualWeight",
				SizedWeight: 0.6, CappedWeight: 0.5, ApprovedWeight: 0.5, MaxAsset: 0.5, MaxGross: 1.5, GrossAfterAssetCap: 0.5, GrossScale: 1}}}}},
	}
	data := realRiskData{}
	simulationRiskView(&data, s)
	if len(data.StrategyLimits) != 1 || data.StrategyLimits[0].Binding != "Asset cap" || data.StrategyLimits[0].ApprovedGross != "50.0%" {
		t.Fatalf("limits = %+v", data.StrategyLimits)
	}
	if len(data.History) != 1 || *data.History[0].Leverage != 0.5 || data.Exposures[0].Cap != "50.0%" || data.Exposures[0].Utilization != 100 {
		t.Fatalf("view = %+v", data)
	}
}
