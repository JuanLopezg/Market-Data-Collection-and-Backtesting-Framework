package provider

import (
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

func TestRuntimeAccountingFeedsPositionsAndEquity(t *testing.T) {
	accounting := pgstore.Accounting{
		Available: true,
		Closed: []pgstore.AccountingPosition{{Coin: "ETHUSDT", Side: "Long", OpenedAt: 20261009, ClosedAt: 20261012, ClosingFillID: 9,
			MaxQuantity: 2, EntryPrice: 1000, EntryValue: 2000, ExitValue: 2200, ExitQuantity: 2, GrossPnL: 200, Fees: 2.1}},
		Open: []pgstore.AccountingPosition{{Coin: "BTCUSDT", Side: "Short", OpenedAt: 20261013, Quantity: -0.5, MaxQuantity: 0.5,
			EntryPrice: 60000, EntryValue: 30000, Fees: 3}},
		Equity: []pgstore.EquityDay{{Date: 20261012, Equity: 100100}, {Date: 20261013, Equity: 99900.5}},
	}

	closed, open := lotsFromAccounting(accounting)
	rows := closedPositionRows(closed)
	if len(rows) != 1 || rows[0].Asset != "ETHUSDT" || !near(rows[0].ExitPrice, 1100) || !near(rows[0].NetPnL, 197.9) || rows[0].ClosedAt != "2026-10-12" {
		t.Fatalf("closed rows: %+v", rows)
	}
	if lot := open["BTCUSDT"]; lot == nil || lot.Side != "Short" || lot.Quantity != -0.5 || lot.Fees != 3 {
		t.Fatalf("open lot: %+v", lot)
	}

	data := realPositionsData{Positions: []realPosition{{Asset: "BTCUSDT", PnLAvailable: true, PnLUSD: 50}}}
	applyPositionLots(&data, closed, open)
	if !data.ClosedAvailable || data.Positions[0].FeesUSD != 3 || data.Positions[0].NetPnLUSD != 47 || data.Positions[0].OpenedAt != "2026-10-13" {
		t.Fatalf("positions: %+v", data.Positions[0])
	}

	overview := realOverviewData{Stats: []realOverviewMetric{{Value: "Not available"}, {}, {}}}
	addRuntimeEquity(&overview, accounting.Equity)
	if !overview.EquityAvailable || len(overview.EquityCurve) != 2 || overview.EquityCurve[1].Label != "2026-10-13" ||
		overview.EquityCurve[1].Equity != 99900.5 || overview.Stats[0].Value != formatUSD(99900.5) {
		t.Fatalf("equity: %+v stats=%+v", overview.EquityCurve, overview.Stats)
	}
}
