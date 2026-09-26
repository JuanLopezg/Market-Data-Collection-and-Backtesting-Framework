package provider

import (
	"testing"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestBuildRealExecutionMapsDurableOrdersAndFillsWithoutInventingQuality(t *testing.T) {
	state := pgstore.RuntimeState{
		UpdatedAt: "2026-09-24T17:00:00.000Z",
		Snapshot:  tradingwire.TradingStateSnapshot{SchemaVersion: 1},
	}
	orders := []tradingwire.PersistedTrackedOrder{
		{OrderID: 12, StrategyID: 1, CreatedAt: 20260924, Coin: "BTC", Side: 0, Quantity: 0.5, Status: 3, FilledQuantity: 0.2, UpdatedAt: 20260924, ExchangeOrderID: "ex-12"},
		{OrderID: 11, StrategyID: 1, CreatedAt: 20260923, Coin: "ETH", Side: 1, Quantity: 2, Status: 6, UpdatedAt: 20260923, LastMessage: "venue rejected"},
	}
	fills := pgstore.FillWindow{TotalRows: 1, TotalCommission: 2.5, Rows: []tradingwire.Fill{
		{FillID: 101, OrderID: 12, StrategyID: 1, Timestamp: 20260924, Coin: "BTC", Side: 0, Quantity: 0.2, Price: 50000, Commission: 2.5},
	}}

	data := buildRealExecution(state, orders, len(orders), fills)
	if data.SourceMode != "REAL" || data.OpenOrders != 1 || data.PartialOrders != 1 || data.RejectCount != 1 || data.FillCount != 1 {
		t.Fatalf("unexpected execution summary: %+v", data)
	}
	if data.LatencyAvailable || data.SlippageAvailable || data.AvgSlippageBpsLabel != "Not available" {
		t.Fatalf("quality fields must remain unavailable when not durable: %+v", data)
	}
	if len(data.Orders) != 2 || data.Orders[0].State != "PARTIAL" || data.Orders[0].AvgFillPriceLabel == "—" || data.Orders[0].FeesLabel != "$2.50" {
		t.Fatalf("unexpected order projection: %+v", data.Orders)
	}
	if data.Orders[0].CycleID != "Not persisted" || data.Orders[0].CorrelationID != "Not persisted" {
		t.Fatalf("trace ids must not be invented: %+v", data.Orders[0])
	}
	if len(data.Rejects) != 1 || data.Rejects[0].Reason != "venue rejected" {
		t.Fatalf("rejection message should use durable last_message: %+v", data.Rejects)
	}
}

func TestMapExecutionStatePreservesCancelRequest(t *testing.T) {
	order := tradingwire.PersistedTrackedOrder{Status: 2, CancelRequested: true}
	if got := mapExecutionState(order); got != "PENDING_CANCEL" {
		t.Fatalf("expected PENDING_CANCEL, got %s", got)
	}
}
