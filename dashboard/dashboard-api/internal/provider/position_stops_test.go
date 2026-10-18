package provider

import (
	"encoding/json"
	"math"
	"strings"
	"testing"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

func stopFixture(id, created uint64, price float64, status int) tradingwire.PersistedTrackedOrder {
	return tradingwire.PersistedTrackedOrder{OrderID: id, CreatedAt: created, Coin: "DOTUSDT", Side: 1,
		Quantity: 10, PositionStopPrice: price, Status: status}
}

func TestPositionStopUsesValuationMarkAndRemainingSourceUnits(t *testing.T) {
	data := buildRealPositions(pgstore.RuntimeState{Snapshot: tradingwire.TradingStateSnapshot{
		AccountCash: 100, AccountPositions: map[string]float64{"DOTUSDT": 10, "SHORT": -1, "NONE": 2},
	}})
	applyPositionValuation(&data, positionValuationInputs{cash: 100, marks: map[string]float64{"DOTUSDT": 5.04, "SHORT": 10, "NONE": 3}})
	order := stopFixture(7, 20261017, 3.53745, 3)
	order.FilledQuantity = 2
	short := order
	short.Coin = "SHORT"
	applyPositionStops(&data, []tradingwire.PersistedTrackedOrder{order, short})
	stop := data.Positions[0].StopLoss
	if stop == nil || stop.OrderID != "7" || stop.Price != 3.53745 || stop.Quantity != 8 || stop.Status != "PARTIAL" || stop.ReplacementInProgress {
		t.Fatalf("unexpected stop: %+v", stop)
	}
	if stop.DistancePct == nil || math.Abs(*stop.DistancePct-(3.53745/5.04-1)*100) > 1e-10 {
		t.Fatalf("distance must use the position valuation mark: %+v", stop)
	}
	for _, position := range data.Positions[1:] {
		if position.StopLoss != nil {
			t.Fatalf("unexpected stop on %s", position.Asset)
		}
		raw, err := json.Marshal(position)
		if err != nil {
			t.Fatal(err)
		}
		if strings.Contains(string(raw), "stopLoss") {
			t.Fatalf("absent stop must be omitted: %s", raw)
		}
	}
	data.Positions[0].ValuationAvailable = false
	applyPositionStops(&data, []tradingwire.PersistedTrackedOrder{order})
	if data.Positions[0].StopLoss.DistancePct != nil {
		t.Fatal("missing mark must not invent distance")
	}
}

func TestPositionStopReplacementSelectsNewestDeterministically(t *testing.T) {
	data := realPositionsData{Positions: []realPosition{{Asset: "DOTUSDT", Side: "Long"}}}
	old := stopFixture(10, 20261016, 70, 2)
	old.CancelRequested = true
	newer := stopFixture(11, 20261017, 60, 1) // Price can move down; newest wins, not highest.
	tie := stopFixture(12, 20261017, 65, 2)
	for _, orders := range [][]tradingwire.PersistedTrackedOrder{{old, newer, tie}, {tie, newer, old}} {
		applyPositionStops(&data, orders)
		stop := data.Positions[0].StopLoss
		if stop == nil || stop.OrderID != "12" || stop.Price != 65 || !stop.ReplacementInProgress || stop.Status != "ACCEPTED" {
			t.Fatalf("unexpected replacement: %+v", stop)
		}
	}
	for _, status := range []int{4, 5, 6, 99} {
		order := tie
		order.Status = status
		applyPositionStops(&data, []tradingwire.PersistedTrackedOrder{order})
		if data.Positions[0].StopLoss != nil {
			t.Fatalf("terminal/unknown status %d is not an open stop", status)
		}
	}
	applyPositionStops(&data, []tradingwire.PersistedTrackedOrder{old})
	if data.Positions[0].StopLoss.Status != "PENDING_CANCEL" || data.Positions[0].StopLoss.ReplacementInProgress {
		t.Fatal("cancel-pending old stop remains visible until terminal")
	}
	newer.PositionStopPrice = 0
	applyPositionStops(&data, []tradingwire.PersistedTrackedOrder{newer})
	if data.Positions[0].StopLoss != nil {
		t.Fatal("market sell is not a stop")
	}
}

func TestPositionStopsLabeledWithoutPendingTradeQuantity(t *testing.T) {
	stop := stopFixture(7, 20261017, 70, 2)
	data := buildRealExecution(pgstore.RuntimeState{}, []tradingwire.PersistedTrackedOrder{stop}, 1, pgstore.FillWindow{}, false)
	if data.Orders[0].OrderType != "STOP" || data.Orders[0].PositionStopPrice != 70 {
		t.Fatal("execution stop label missing")
	}
	open := reconciliationOpenOrders([]tradingwire.PersistedTrackedOrder{stop})
	if len(open) != 1 || open[0].OrderType != "STOP" {
		t.Fatal("open-order stop label missing")
	}
	if pendingSignedQuantities([]tradingwire.PersistedTrackedOrder{stop})[stop.Coin] != 0 {
		t.Fatal("resting protection is not a pending trade")
	}
}

func TestMissingPositionStopGraceAndReplacement(t *testing.T) {
	var observer positionStopAlertState
	now := time.Date(2026, 10, 17, 12, 0, 0, 0, time.UTC)
	state := tradingwire.TradingStateSnapshot{AccountPositions: map[string]float64{"DOTUSDT": 10, "SHORT": -2, "FLAT": 0}}
	if got := observer.observe(true, &state, now); len(got) != 0 {
		t.Fatal("new entry must have grace")
	}
	if got := observer.observe(true, &state, now.Add(10*time.Minute-time.Second)); len(got) != 0 {
		t.Fatal("grace has not elapsed")
	}
	alerts := observer.observe(true, &state, now.Add(10*time.Minute))
	if len(alerts) != 1 || alerts[0].Title != "DOTUSDT: no stop-loss" || alerts[0].Severity != "WARN" || alerts[0].EventType != "POSITION_STOP_MISSING" {
		t.Fatalf("unexpected missing-stop warning: %+v", alerts)
	}
	old := stopFixture(10, 20261017, 70, 2)
	old.CancelRequested = true
	state.Orders = []tradingwire.PersistedTrackedOrder{old, stopFixture(11, 20261017, 60, 1)}
	if got := observer.observe(true, &state, now.Add(11*time.Minute)); len(got) != 0 {
		t.Fatal("replacement still has old accepted protection")
	}
	state.Orders[0].Status = 5
	if got := observer.observe(true, &state, now.Add(12*time.Minute)); len(got) != 0 {
		t.Fatal("missing protection starts a fresh grace")
	}
	if got := observer.observe(true, &state, now.Add(22*time.Minute)); len(got) != 1 {
		t.Fatal("stuck submitted stop is not accepted protection")
	}
	state.AccountPositions["DOTUSDT"] = 0
	if got := observer.observe(true, &state, now.Add(23*time.Minute)); len(got) != 0 {
		t.Fatal("flat position resolves warning")
	}
	state.AccountPositions["DOTUSDT"] = 10
	if got := observer.observe(true, &state, now.Add(24*time.Minute)); len(got) != 0 {
		t.Fatal("re-entry gets new grace")
	}
}

func TestMissingPositionStopPaperUnavailableAndRestart(t *testing.T) {
	var observer positionStopAlertState
	now := time.Now()
	state := tradingwire.TradingStateSnapshot{AccountPositions: map[string]float64{"DOTUSDT": 10}}
	observer.observe(false, &state, now)
	if got := observer.observe(false, &state, now.Add(time.Hour)); len(got) != 0 {
		t.Fatal("PAPER unchanged")
	}
	observer.observe(true, &state, now)
	observer.observe(true, nil, now.Add(9*time.Minute))
	if got := observer.observe(true, &state, now.Add(11*time.Minute)); len(got) != 0 {
		t.Fatal("unavailable evidence breaks observed grace")
	}
	state.AccountPositions["DOTUSDT"] = 12
	if got := observer.observe(true, &state, now.Add(22*time.Minute)); len(got) != 0 {
		t.Fatal("newly filled size gets fresh grace")
	}
	restarted := positionStopAlertState{}
	if got := restarted.observe(true, &state, now.Add(time.Hour)); len(got) != 0 {
		t.Fatal("observer restart starts fresh grace")
	}
}
