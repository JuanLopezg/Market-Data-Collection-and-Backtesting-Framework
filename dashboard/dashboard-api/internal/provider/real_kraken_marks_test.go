package provider

import (
	"math"
	"testing"

	krakenpublic "control-dashboard-api/internal/integration/kraken"
	"control-dashboard-api/internal/integration/symbolmap"
)

func approxEqual(a, b float64) bool { return math.Abs(a-b) <= 1e-9*math.Max(1, math.Abs(b)) }

func TestKrakenMarksUseTheSymbolMapScale(t *testing.T) {
	table, err := symbolmap.Load(writeSymbolMap(t, testSymbolMapJSON), "kraken-futures")
	if err != nil {
		t.Fatal(err)
	}
	marks := krakenMarksFromTickers(table, map[string]krakenpublic.Ticker{
		"PF_XBTUSD":  {Symbol: "PF_XBTUSD", MarkPrice: 80000, FundingRate: 0.8, FundingKnown: true},
		"PF_PEPEUSD": {Symbol: "PF_PEPEUSD", MarkPrice: 0.000004, FundingRate: -0.000000002, FundingKnown: true},
		"PF_ETHUSD":  {Symbol: "PF_ETHUSD", MarkPrice: 2500}, // no funding field
		"PF_HALTUSD": {Symbol: "PF_HALTUSD", MarkPrice: 3, Suspended: true},
	})
	// 1000PEPEUSDT is priced per 1,000 PEPE: the fixed scale, never the price ratio.
	if !approxEqual(marks["1000PEPEUSDT"].Price, 0.004) || !approxEqual(marks["BTCUSDT"].Price, 80000) {
		t.Fatalf("marks = %+v", marks)
	}
	// Funding is relative to the mark, so the scale does not matter: 0.8/80,000 = 0.001%/h.
	if !approxEqual(marks["BTCUSDT"].FundingHourly, 0.00001) || !approxEqual(marks["1000PEPEUSDT"].FundingHourly, -0.0005) {
		t.Fatalf("funding = %+v", marks)
	}
	if marks["ETHUSDT"].FundingKnown {
		t.Fatal("a missing funding rate must stay unknown, not zero")
	}
	if _, listed := marks["HALTUSDT"]; listed {
		t.Fatal("a suspended market must not price a position")
	}
}

func TestLiveValuationCountsCollateralAndFunding(t *testing.T) {
	data := realPositionsData{Positions: []realPosition{
		{Asset: "NEARUSDT", Side: "Long", EffectiveQty: 4},
		{Asset: "WLDUSDT", Side: "Long", EffectiveQty: 43},
	}}
	applyPositionValuation(&data, positionValuationInputs{
		cash: 40, collateral: 120, status: "ALIGNED",
		marks:   map[string]float64{"NEARUSDT": 5, "WLDUSDT": 0.5},
		entries: map[string]float64{"NEARUSDT": 5.05, "WLDUSDT": 0.5},
	})
	// Equity = cash 40 + NEAR 20 + WLD 21.5 + BTC collateral 120 = 201.5.
	if data.ValuationState != "AVAILABLE" || !approxEqual(data.equityValue, 201.5) || !approxEqual(data.collateralValue, 120) {
		t.Fatalf("valuation = %+v", data)
	}
	if !approxEqual(data.Positions[0].CurrentWeightPct, 20/201.5*100) || !approxEqual(data.Positions[0].PnLUSD, -0.2) {
		t.Fatalf("NEAR row = %+v", data.Positions[0])
	}
	applyFundingRates(&data, map[string]krakenMark{"NEARUSDT": {Price: 5, FundingHourly: 0.00001, FundingKnown: true}})
	if !data.Positions[0].FundingRateAvailable || !approxEqual(data.Positions[0].FundingRateAnnualPct, 0.00001*24*365*100) ||
		data.Positions[1].FundingRateAvailable {
		t.Fatalf("funding rates = %+v", data.Positions)
	}

	// Paid funding enters Net PnL: unrealized -0.2, fees 0.01, funding -0.03 -> -0.24.
	data.FundingAvailable = true
	applyPositionLots(&data, nil, map[string]*positionLot{"NEARUSDT": {Asset: "NEARUSDT", Fees: 0.01, Funding: -0.03}})
	if !data.Positions[0].FundingAvailable || !approxEqual(data.Positions[0].FundingUSD, -0.03) || !approxEqual(data.Positions[0].NetPnLUSD, -0.24) {
		t.Fatalf("net PnL row = %+v", data.Positions[0])
	}
}
