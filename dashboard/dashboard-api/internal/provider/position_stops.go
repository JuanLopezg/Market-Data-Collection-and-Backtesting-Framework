package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strconv"
	"sync"
	"time"

	"control-dashboard-api/internal/tradingwire"
)

type positionStopLoss struct {
	OrderID               string   `json:"orderId"`
	Price                 float64  `json:"price"`
	Quantity              float64  `json:"quantity"`
	DistancePct           *float64 `json:"distancePct,omitempty"`
	Status                string   `json:"status"`
	ReplacementInProgress bool     `json:"replacementInProgress"`
}

func positionStopOrderType(order tradingwire.PersistedTrackedOrder) string {
	if order.PositionStopPrice > 0 {
		return "STOP"
	}
	return ""
}

func openPositionStop(order tradingwire.PersistedTrackedOrder) bool {
	return order.Side == 1 && finitePositive(order.PositionStopPrice) &&
		order.Status >= 0 && order.Status <= 3 &&
		finitePositive(order.Quantity-order.FilledQuantity)
}

func positionStopStatus(order tradingwire.PersistedTrackedOrder) string {
	if order.CancelRequested {
		return "PENDING_CANCEL"
	}
	return [...]string{"CREATED", "SUBMITTED", "ACCEPTED", "PARTIAL"}[order.Status]
}

func applyPositionStops(data *realPositionsData, orders []tradingwire.PersistedTrackedOrder) {
	newest := map[string]tradingwire.PersistedTrackedOrder{}
	counts := map[string]int{}
	for _, order := range orders {
		if !openPositionStop(order) {
			continue
		}
		previous, exists := newest[order.Coin]
		if !exists || order.CreatedAt > previous.CreatedAt ||
			(order.CreatedAt == previous.CreatedAt && order.OrderID > previous.OrderID) {
			newest[order.Coin] = order
		}
		counts[order.Coin]++
	}
	for i := range data.Positions {
		position := &data.Positions[i]
		position.StopLoss = nil
		order, exists := newest[position.Asset]
		if !exists || position.Side != "Long" {
			continue
		}
		stop := &positionStopLoss{
			OrderID: strconv.FormatUint(order.OrderID, 10), Price: order.PositionStopPrice,
			Quantity: order.Quantity - order.FilledQuantity, Status: positionStopStatus(order),
			ReplacementInProgress: counts[position.Asset] > 1,
		}
		if position.ValuationAvailable && finitePositive(position.CurrentPrice) {
			distance := (stop.Price/position.CurrentPrice - 1) * 100
			if !math.IsInf(distance, 0) && !math.IsNaN(distance) {
				stop.DistancePct = &distance
			}
		}
		position.StopLoss = stop
	}
}

const positionStopGrace = 10 * time.Minute

type missingPositionStop struct {
	since    time.Time
	quantity float64
}

// Wall-clock observation only: daily business timestamps cannot measure a ten-minute gap.
// Restart or unavailable evidence starts a fresh grace; trading state is never changed.
type positionStopAlertState struct {
	mu      sync.Mutex
	missing map[string]missingPositionStop
}

func (s *positionStopAlertState) observe(live bool, snapshot *tradingwire.TradingStateSnapshot, now time.Time) []realOperationalAlert {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !live || snapshot == nil {
		s.missing = nil
		return nil
	}
	protected := map[string]bool{}
	for _, order := range snapshot.Orders {
		// A submitted replacement is not venue protection yet. An accepted old stop,
		// including one awaiting cancellation, protects until its terminal update.
		if openPositionStop(order) && order.Status >= 2 {
			protected[order.Coin] = true
		}
	}
	next := map[string]missingPositionStop{}
	alerts := []realOperationalAlert{}
	for coin, quantity := range snapshot.AccountPositions {
		if !finitePositive(quantity) || quantity <= 1e-12 || protected[coin] {
			continue
		}
		gap, exists := s.missing[coin]
		if !exists || now.Before(gap.since) || quantity > gap.quantity+1e-12 {
			gap.since = now
		}
		gap.quantity = quantity
		next[coin] = gap
		if now.Sub(gap.since) < positionStopGrace {
			continue
		}
		alerts = append(alerts, realOperationalAlert{
			ID: "derived-position-no-stop-" + coin, Timestamp: gap.since.UTC().Format(time.RFC3339),
			Severity: "WARN", Status: "ACTIVE", Service: "Execution", Asset: coin,
			EventType: "POSITION_STOP_MISSING", Title: coin + ": no stop-loss",
			Detail:        fmt.Sprintf("Long position has had no observed accepted stop-loss for at least 10 minutes (%s units).", formatQuantity(quantity)),
			LinkedContext: "Open Positions",
		})
	}
	s.missing = next
	sort.Slice(alerts, func(i, j int) bool { return alerts[i].ID < alerts[j].ID })
	return alerts
}

func (p *Real) missingPositionStopAlerts(ctx context.Context) []realOperationalAlert {
	if !p.krakenLive() {
		return p.positionStopAlerts.observe(false, nil, time.Now())
	}
	state, err := p.postgres.RuntimeState(ctx)
	if err != nil {
		return p.positionStopAlerts.observe(true, nil, time.Now())
	}
	return p.positionStopAlerts.observe(true, &state.Snapshot, time.Now())
}
