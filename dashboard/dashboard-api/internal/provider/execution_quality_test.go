package provider

import (
	"math"
	"strings"
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestExecutionSlippageUsesPersistedReferencesAndWeightedFills(t *testing.T) {
	orders := []tradingwire.PersistedTrackedOrder{
		{OrderID: 1, Coin: "BTC", ReferencePrice: 100, Quantity: 2, FilledQuantity: 2, Status: 4},
		{OrderID: 2, Coin: "BTC", Side: 1, ReferencePrice: 100, Quantity: 1, FilledQuantity: 1, Status: 4},
		{OrderID: 3, Coin: "1000PEPEUSDT", Side: 1, ReferencePrice: 0.01, PositionStopPrice: 0.007, Quantity: 1000, FilledQuantity: 1000, Status: 4},
		{OrderID: 4, Coin: "BTC", Quantity: 1, FilledQuantity: 1, Status: 4},   // Old order: no reference.
		{OrderID: 5, Coin: "BTC", ReferencePrice: 100, Quantity: 1, Status: 2}, // Unfilled stop/trade.
	}
	fills := []tradingwire.Fill{
		{FillID: 1, OrderID: 1, Coin: "BTC", Timestamp: 20261017, Quantity: 1, Price: 101, Commission: 0.003, FeeRecorded: true},
		{FillID: 2, OrderID: 1, Coin: "BTC", Timestamp: 20261017, Quantity: 1, Price: 99, Commission: 0.002, FeeRecorded: true},
		{FillID: 3, OrderID: 2, Coin: "BTC", Side: 1, Timestamp: 20261018, Quantity: 1, Price: 99, FeeRecorded: true},
		{FillID: 4, OrderID: 3, Coin: "1000PEPEUSDT", Side: 1, Timestamp: 20261018, Quantity: 1000, Price: 0.0063, FeeRecorded: true},
		{FillID: 5, OrderID: 4, Coin: "BTC", Timestamp: 20261018, Quantity: 1, Price: 101, FeeRecorded: true},
	}
	data := buildRealExecution(pgstore.RuntimeState{}, orders, len(orders), pgstore.FillWindow{Rows: fills, TotalRows: 5, TotalCommission: 0.005}, true)
	if !data.SlippageAvailable || data.SlippageFillCount != 4 || len(data.SlippageHistory) != 2 {
		t.Fatalf("unexpected quality coverage: %+v", data)
	}
	// Partial fills use VWAP for the order row; the overall average uses actual USD notional.
	if data.Orders[0].SlippageBpsLabel != "0.00 bps" || data.Orders[0].FeesLabel != "$0.005" {
		t.Fatalf("partial fill aggregation lost: %+v", data.Orders[0])
	}
	if math.Abs(data.SlippageHistory[0].Bps) > 1e-8 || math.Abs(data.SlippageHistory[1].Bps-1100) > 1e-8 {
		t.Fatalf("daily result must sum each fill's bps: %+v", data.SlippageHistory)
	}
	if data.AvgSlippageBpsLabel != "+53.72 bps" || data.BestSlippageBpsLabel != "-100.00 bps" || data.WorstSlippageBpsLabel != "+1000.00 bps" {
		t.Fatalf("unexpected overall fill weighting: %+v", data)
	}
	if math.Abs(data.Orders[1].SlippageBps-100) > 1e-8 || math.Abs(data.Orders[2].SlippageBps-1000) > 1e-8 {
		t.Fatalf("SELL sign or scaled stop trigger incorrect: %+v", data.Orders)
	}
	if data.Orders[2].ExpectedPriceLabel != "$0.00700000" || !strings.Contains(data.Orders[2].ExpectedPriceSource, "Stop trigger") {
		t.Fatalf("stop must use its trigger, not the daily close: %+v", data.Orders[2])
	}
	if data.Orders[3].ExpectedPriceLabel != "—" || data.Orders[3].SlippageBpsLabel != "—" || data.Orders[4].SlippageBpsLabel != "—" {
		t.Fatal("missing references or unfilled orders must not manufacture slippage")
	}
	if data.TotalFeesLabel != "$0.005" || data.LatencyAvailable {
		t.Fatalf("small fees must remain visible and latency unavailable: %+v", data)
	}
}

func TestRealExecutionSlippageDailySum(t *testing.T) {
	orders := []tradingwire.PersistedTrackedOrder{{OrderID: 1, Coin: "BTC", ReferencePrice: 100}}
	fills := []tradingwire.Fill{
		{OrderID: 1, Coin: "BTC", Timestamp: 20261018, Quantity: 1, Price: 100.10},
		{OrderID: 1, Coin: "BTC", Timestamp: 20261018, Quantity: 3, Price: 99.96},
		{OrderID: 1, Coin: "BTC", Timestamp: 20261019, Quantity: 1, Price: 100.02},
	}
	got := realExecutionSlippage(orders, fills)
	if got.fills != 3 || len(got.daily) != 2 {
		t.Fatalf("unexpected fill coverage: %+v", got)
	}
	if got.daily[0].Label != "2026-10-18" || got.daily[0].Fills != 2 || math.Abs(got.daily[0].Bps-6) > 1e-8 {
		t.Fatalf("10 and -4 bps must sum to 6 regardless of notional: %+v", got.daily)
	}
	if got.daily[1].Label != "2026-10-19" || got.daily[1].Fills != 1 || math.Abs(got.daily[1].Bps-2) > 1e-8 {
		t.Fatalf("fills from another day must stay separate: %+v", got.daily)
	}
}

func TestExecutionFeesPendingAndPaperZero(t *testing.T) {
	orders := []tradingwire.PersistedTrackedOrder{{OrderID: 1, Coin: "BTC", Quantity: 1, FilledQuantity: 1, ReferencePrice: 100, Status: 4}}
	fills := pgstore.FillWindow{TotalRows: 1, PendingFeeRows: 1, Rows: []tradingwire.Fill{{FillID: 1, OrderID: 1, Coin: "BTC", Quantity: 1, Price: 100, Timestamp: 20261018}}}
	live := buildRealExecution(pgstore.RuntimeState{}, orders, 1, fills, true)
	if live.Orders[0].FeesLabel != "Pending" || live.PartialFills[0].FeesLabel != "Pending" || live.FeesPendingCount != 1 {
		t.Fatalf("unreported Kraken fee must remain pending: %+v", live)
	}
	paper := buildRealExecution(pgstore.RuntimeState{}, orders, 1, fills, false)
	if paper.Orders[0].FeesLabel != "$0.00" || paper.FeesPendingCount != 0 {
		t.Fatalf("PAPER zero fee must remain zero: %+v", paper)
	}
	fills.Rows[0].FeeRecorded = true // A venue can explicitly report a zero fee.
	fills.PendingFeeRows = 0
	live = buildRealExecution(pgstore.RuntimeState{}, orders, 1, fills, true)
	if live.Orders[0].FeesLabel != "$0.00" || live.FeesPendingCount != 0 {
		t.Fatal("recorded zero fee must not be pending")
	}
}

func TestExecutionFeeLabelsKeepSmallAmounts(t *testing.T) {
	for _, test := range []struct {
		amount  float64
		pending bool
		want    string
	}{
		{0.0068, false, "$0.0068"},
		{0.0211, false, "$0.0211"},
		{0.00000001, false, "$0.00000001"},
		{10, false, "$10.00"},
		{0.1, false, "$0.10"},
		{0.005, true, "$0.005 + pending"},
	} {
		if got := executionFeesLabel(test.amount, test.pending); got != test.want {
			t.Fatalf("fee label for %v: got %q, want %q", test.amount, got, test.want)
		}
	}
}

func TestExecutionTruncatedFillsSuppressPartialQuality(t *testing.T) {
	order := tradingwire.PersistedTrackedOrder{OrderID: 1, Coin: "BTC", ReferencePrice: 100, Quantity: 2000, FilledQuantity: 2000, Status: 4}
	fills := pgstore.FillWindow{TotalRows: 2000, TotalCommission: 0.12345}
	for i := 0; i < maxExecutionFills; i++ {
		fills.Rows = append(fills.Rows, tradingwire.Fill{FillID: uint64(i + 1), OrderID: 1, Coin: "BTC", Timestamp: 20261018, Quantity: 1, Price: 101})
	}
	data := buildRealExecution(pgstore.RuntimeState{}, []tradingwire.PersistedTrackedOrder{order}, 1, fills, false)
	if data.SlippageAvailable || len(data.SlippageHistory) != 0 || data.Orders[0].FeesLabel != "Bounded window" || data.TotalFeesLabel != "$0.12345" {
		t.Fatalf("bounded details must not look complete: %+v", data)
	}
}

func TestExecutionQualityRejectsInvalidAndMismatchedInputs(t *testing.T) {
	for _, value := range []float64{0, -1, math.NaN(), math.Inf(1)} {
		if _, ok := slippageBpsFor("BUY", value, 10); ok {
			t.Fatalf("invalid reference accepted: %v", value)
		}
	}
	order := tradingwire.PersistedTrackedOrder{OrderID: 1, Coin: "BTC", ReferencePrice: 100}
	for _, fill := range []tradingwire.Fill{
		{OrderID: 2, Coin: "BTC", Quantity: 1, Price: 101, Timestamp: 20261018},
		{OrderID: 1, Coin: "ETH", Quantity: 1, Price: 101, Timestamp: 20261018},
		{OrderID: 1, Coin: "BTC", Side: 1, Quantity: 1, Price: 101, Timestamp: 20261018},
		{OrderID: 1, Coin: "BTC", Quantity: 1, Price: 101, Timestamp: 0},
	} {
		quality := realExecutionSlippage([]tradingwire.PersistedTrackedOrder{order}, []tradingwire.Fill{fill})
		if quality.fills != 0 {
			t.Fatalf("invalid fill produced quality: %+v", fill)
		}
	}
}
