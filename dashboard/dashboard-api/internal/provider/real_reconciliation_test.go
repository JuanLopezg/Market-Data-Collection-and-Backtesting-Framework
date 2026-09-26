package provider

import (
	"testing"

	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestCompareRuntimeAndExchangeMirrorsCoreReconciler(t *testing.T) {
	local := tradingwire.TradingStateSnapshot{
		SchemaVersion:    1,
		AccountCash:      100,
		AccountPositions: map[string]float64{"BTC": 1},
		Orders:           []tradingwire.PersistedTrackedOrder{{OrderID: 7, Coin: "ETH", Side: 0, Quantity: 2, FilledQuantity: .5, Status: 2, ExchangeOrderID: "ex-7"}},
	}
	exchange := tradingwire.ExchangeSnapshot{
		Timestamp:  20260924,
		Cash:       100,
		Positions:  map[string]float64{"BTC": 1},
		OpenOrders: []tradingwire.ExchangeOpenOrderSnapshot{{LocalOrderID: 7, ExchangeOrderID: "ex-7", Coin: "ETH", Side: 0, Quantity: 2, FilledQuantity: .5}},
	}
	comparison := compareRuntimeAndExchange(local, exchange)
	if len(comparison.Issues) != 0 {
		t.Fatalf("expected clean reconciliation, got %+v", comparison.Issues)
	}

	exchange.Cash = 99
	exchange.Positions["BTC"] = .9
	exchange.OpenOrders[0].FilledQuantity = .25
	comparison = compareRuntimeAndExchange(local, exchange)
	if len(comparison.Issues) != 3 {
		t.Fatalf("expected cash, position and order mismatch, got %+v", comparison.Issues)
	}
}

func TestBuildRealReconciliationMarksStaleEvidencePending(t *testing.T) {
	state := pgstore.RuntimeState{
		UpdatedAt: "2026-09-24T18:00:10Z",
		Snapshot:  tradingwire.TradingStateSnapshot{SchemaVersion: 1, AccountCash: 100, AccountPositions: map[string]float64{}},
	}
	message := natsdiag.JetStreamMessage{Time: "2026-09-24T18:00:00Z", Sequence: 12}
	event := tradingwire.ExchangeSnapshotEvent{Snapshot: tradingwire.ExchangeSnapshot{Timestamp: 20260924, Cash: 99, Positions: map[string]float64{}}}
	data := buildRealReconciliation(state, message, event)
	if data.Status != "PENDING" || data.EvidenceFresh {
		t.Fatalf("expected stale evidence to be pending: %+v", data)
	}
	if len(data.Issues) != 1 {
		t.Fatalf("comparison issues should remain visible as evidence, got %+v", data.Issues)
	}
}

func TestBuildRealReconciliationFreshMismatchBlocks(t *testing.T) {
	state := pgstore.RuntimeState{
		UpdatedAt: "2026-09-24T18:00:00Z",
		Snapshot:  tradingwire.TradingStateSnapshot{SchemaVersion: 1, AccountCash: 100, AccountPositions: map[string]float64{"BTC": 1}},
	}
	message := natsdiag.JetStreamMessage{Time: "2026-09-24T18:00:01Z", Sequence: 13}
	event := tradingwire.ExchangeSnapshotEvent{Snapshot: tradingwire.ExchangeSnapshot{Timestamp: 20260924, Cash: 100, Positions: map[string]float64{"BTC": .5}}}
	data := buildRealReconciliation(state, message, event)
	if data.Status != "BLOCKED" || !data.EvidenceFresh || len(data.Issues) != 1 || data.Rows[0].Status != "BLOCKED" {
		t.Fatalf("expected fresh mismatch to block: %+v", data)
	}
}
