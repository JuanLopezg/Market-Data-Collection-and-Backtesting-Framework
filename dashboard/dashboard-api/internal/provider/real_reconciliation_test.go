package provider

import (
	"math"
	"testing"
	"time"

	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func TestReconciliationQuantityToleranceScalesWithSize(t *testing.T) {
	venue := 711394.39137621
	noisy := venue
	for i := 0; i < 3; i++ {
		noisy = math.Nextafter(noisy, math.Inf(1))
	}
	if math.Abs(noisy-venue) <= reconciliationQuantityTolerance || !sameReconciliationQuantity(noisy, venue) {
		t.Fatalf("rounding noise on a large position must not be drift")
	}
	if sameReconciliationQuantity(1e10, 1e10-1) {
		t.Fatalf("one unit missing must still be drift")
	}
	if sameReconciliationQuantity(0.5, 0.499) || !sameReconciliationQuantity(0.5, 0.5+5e-11) {
		t.Fatalf("small positions keep the absolute tolerance")
	}
}

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
	comparison := compareRuntimeAndExchange(local, exchange, reconciliationCashTolerance)
	if len(comparison.Issues) != 0 {
		t.Fatalf("expected clean reconciliation, got %+v", comparison.Issues)
	}

	exchange.Cash = 99
	exchange.Positions["BTC"] = .9
	exchange.OpenOrders[0].FilledQuantity = .25
	comparison = compareRuntimeAndExchange(local, exchange, reconciliationCashTolerance)
	if len(comparison.Issues) != 3 {
		t.Fatalf("expected cash, position and order mismatch, got %+v", comparison.Issues)
	}
}

func TestKrakenLiveCashToleranceMatchesExecutionState(t *testing.T) {
	paper := NewReal(RealConfig{RuntimeMode: "PAPER", ExecutionVenue: "SIMULATED"})
	live := NewReal(RealConfig{RuntimeMode: "LIVE", ExecutionVenue: "KRAKEN"})
	if paper.reconciliationTolerances().cash != 1e-6 || live.reconciliationTolerances().cash != 0.5 {
		t.Fatalf("cash tolerances paper %v live %v", paper.reconciliationTolerances().cash, live.reconciliationTolerances().cash)
	}
	local := tradingwire.TradingStateSnapshot{SchemaVersion: 1, AccountCash: 100, AccountPositions: map[string]float64{}}
	exchange := tradingwire.ExchangeSnapshot{Timestamp: 20261014, Cash: 100.3, Positions: map[string]float64{}}
	if len(compareRuntimeAndExchange(local, exchange, live.reconciliationTolerances().cash).Issues) != 0 {
		t.Fatalf("0.30 USD of venue rounding must be accepted in LIVE")
	}
	exchange.Cash = 101
	if len(compareRuntimeAndExchange(local, exchange, live.reconciliationTolerances().cash).Issues) != 1 {
		t.Fatalf("1 USD missing must still be drift in LIVE")
	}
}

func TestBuildRealReconciliationMarksStaleEvidencePending(t *testing.T) {
	state := pgstore.RuntimeState{
		UpdatedAt: "2026-09-24T18:00:10Z",
		Snapshot:  tradingwire.TradingStateSnapshot{SchemaVersion: 1, AccountCash: 100, AccountPositions: map[string]float64{}},
	}
	message := natsdiag.JetStreamMessage{Time: "2026-09-24T18:00:00Z", Sequence: 12}
	event := tradingwire.ExchangeSnapshotEvent{Snapshot: tradingwire.ExchangeSnapshot{Timestamp: 20260924, Cash: 99, Positions: map[string]float64{}}}
	data := buildRealReconciliation(state, message, event, reconciliationTolerance{reconciliationCashTolerance, reconciliationToleranceLabel}, testNow)
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
	data := buildRealReconciliation(state, message, event, reconciliationTolerance{reconciliationCashTolerance, reconciliationToleranceLabel}, testNow)
	if data.Status != "BLOCKED" || !data.EvidenceFresh || len(data.Issues) != 1 || data.Rows[0].Status != "BLOCKED" {
		t.Fatalf("expected fresh mismatch to block: %+v", data)
	}
}

var testNow = time.Date(2026, 9, 24, 18, 30, 0, 0, time.UTC)

func TestBuildRealReconciliationRecentAgreeingEvidenceIsClean(t *testing.T) {
	// A funding entry updated the local state a minute after the hourly exchange check.
	state := pgstore.RuntimeState{
		UpdatedAt: "2026-09-24T18:00:54Z",
		Snapshot:  tradingwire.TradingStateSnapshot{SchemaVersion: 1, AccountCash: 100, AccountPositions: map[string]float64{"BTC": 1}},
	}
	message := natsdiag.JetStreamMessage{Time: "2026-09-24T18:00:01Z", Sequence: 14}
	event := tradingwire.ExchangeSnapshotEvent{Snapshot: tradingwire.ExchangeSnapshot{Timestamp: 20260924, Cash: 100, Positions: map[string]float64{"BTC": 1}}}
	tolerance := reconciliationTolerance{reconciliationCashTolerance, reconciliationToleranceLabel}
	data := buildRealReconciliation(state, message, event, tolerance, testNow)
	if data.Status != "CLEAN" || data.EvidenceFresh || data.Rows[0].Status != "ALIGNED" {
		t.Fatalf("expected recent agreeing evidence to be clean: %+v", data)
	}
	if old := buildRealReconciliation(state, message, event, tolerance, testNow.Add(3*time.Hour)); old.Status != "PENDING" || old.Rows[0].Status != "PENDING" {
		t.Fatalf("expected evidence older than 3 hours to be pending: %+v", old)
	}
}
