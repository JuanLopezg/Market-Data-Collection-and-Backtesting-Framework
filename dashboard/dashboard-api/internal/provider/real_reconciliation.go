package provider

import (
	"context"
	"errors"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

	natsdiag "control-dashboard-api/internal/integration/nats"
	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

const (
	reconciliationCashTolerance     = 1e-6
	reconciliationQuantityTolerance = 1e-10
)

type realReconciliationData struct {
	Status                    string                    `json:"status"`
	LastChecked               string                    `json:"lastChecked"`
	Tolerance                 string                    `json:"tolerance"`
	TargetPortfolioValue      string                    `json:"targetPortfolioValue"`
	LocalPortfolioValue       string                    `json:"localPortfolioValue"`
	ExchangePortfolioValue    string                    `json:"exchangePortfolioValue"`
	Rows                      []realReconciliationRow   `json:"rows"`
	OpenOrders                []realOpenOrderSummary    `json:"openOrders"`
	SourceMode                string                    `json:"sourceMode"`
	SourceUpdatedAt           string                    `json:"sourceUpdatedAt"`
	SourceNote                string                    `json:"sourceNote"`
	ComparisonAvailable       bool                      `json:"comparisonAvailable"`
	ExchangeEvidenceTime      string                    `json:"exchangeEvidenceTime"`
	ExchangeEvidenceSequence  uint64                    `json:"exchangeEvidenceSequence"`
	ExchangeSnapshotTimestamp uint64                    `json:"exchangeSnapshotTimestamp"`
	EvidenceFresh             bool                      `json:"evidenceFresh"`
	Issues                    []realReconciliationIssue `json:"issues"`
}

type realReconciliationRow struct {
	Asset             string  `json:"asset"`
	TargetQty         float64 `json:"targetQty"`
	TargetQtyLabel    string  `json:"targetQtyLabel"`
	TargetAvailable   bool    `json:"targetAvailable"`
	EffectiveQty      float64 `json:"effectiveQty"`
	EffectiveQtyLabel string  `json:"effectiveQtyLabel"`
	LocalQty          float64 `json:"localQty"`
	LocalQtyLabel     string  `json:"localQtyLabel"`
	ExchangeQty       float64 `json:"exchangeQty"`
	ExchangeQtyLabel  string  `json:"exchangeQtyLabel"`
	ExchangeAvailable bool    `json:"exchangeAvailable"`
	DeltaQty          float64 `json:"deltaQty"`
	DeltaQtyLabel     string  `json:"deltaQtyLabel"`
	PendingQty        float64 `json:"pendingQty"`
	PendingQtyLabel   string  `json:"pendingQtyLabel"`
	Status            string  `json:"status"`
	Explanation       string  `json:"explanation"`
}

type realOpenOrderSummary struct {
	OrderID    string  `json:"orderId"`
	Asset      string  `json:"asset"`
	Side       string  `json:"side"`
	Quantity   float64 `json:"quantity"`
	Remaining  float64 `json:"remaining"`
	Age        string  `json:"age"`
	State      string  `json:"state"`
	ExchangeID string  `json:"exchangeId"`
}

type realReconciliationIssue struct {
	Kind          string  `json:"kind"`
	Asset         string  `json:"asset"`
	OrderID       string  `json:"orderId"`
	LocalValue    float64 `json:"localValue"`
	ExchangeValue float64 `json:"exchangeValue"`
	Message       string  `json:"message"`
}

type reconciliationComparison struct {
	Issues []realReconciliationIssue
}

func (p *Real) reconciliation(ctx context.Context) (realReconciliationData, error) {
	state, err := p.postgres.RuntimeState(ctx)
	if err != nil {
		return realReconciliationData{}, fmt.Errorf("read durable reconciliation state: %w", err)
	}

	message, err := p.jetstream.LastBySubject(ctx, natsdiag.SubjectExchangeSnapshot)
	if err != nil {
		if errors.Is(err, natsdiag.ErrJetStreamMessageNotFound) {
			return buildReconciliationWithoutExchange(state, "No retained execution.exchange.snapshot.v1 event exists in ALGOTRADING_RUNTIME yet."), nil
		}
		return buildReconciliationWithoutExchange(state, "Exchange evidence could not be read from JetStream: "+err.Error()), nil
	}

	exchangeEvent, err := tradingwire.DecodeExchangeSnapshotEvent(message.Data)
	if err != nil {
		return buildReconciliationWithoutExchange(state, "The latest retained exchange snapshot could not be decoded safely: "+err.Error()), nil
	}
	return buildRealReconciliation(state, message, exchangeEvent), nil
}

func buildReconciliationWithoutExchange(state pgstore.RuntimeState, reason string) realReconciliationData {
	rows := buildReconciliationRows(state.Snapshot, nil, false, false)
	return realReconciliationData{
		Status:                 "PENDING",
		LastChecked:            "Awaiting exchange evidence",
		Tolerance:              "cash 1e-6 / quantity 1e-10 (runtime Reconciler defaults)",
		TargetPortfolioValue:   "Not available",
		LocalPortfolioValue:    "Cash " + formatUSD(state.Snapshot.AccountCash) + " + positions unvalued",
		ExchangePortfolioValue: "Not available",
		Rows:                   rows,
		OpenOrders:             reconciliationOpenOrders(state.Snapshot.Orders),
		SourceMode:             "REAL",
		SourceUpdatedAt:        state.UpdatedAt,
		SourceNote:             reason + " The dashboard does not request a fresh venue snapshot and does not infer exchange truth. It only reads retained canonical evidence.",
		ComparisonAvailable:    false,
		EvidenceFresh:          false,
		Issues:                 []realReconciliationIssue{},
	}
}

func buildRealReconciliation(state pgstore.RuntimeState, message natsdiag.JetStreamMessage, event tradingwire.ExchangeSnapshotEvent) realReconciliationData {
	fresh := exchangeEvidenceFresh(state.UpdatedAt, message.Time)
	comparison := compareRuntimeAndExchange(state.Snapshot, event.Snapshot)
	status := "CLEAN"
	if !fresh {
		status = "PENDING"
	} else if len(comparison.Issues) > 0 {
		status = "BLOCKED"
	}

	note := "Local truth comes from trading_runtime_state.snapshot. Exchange truth comes from the latest retained execution.exchange.snapshot.v1 event in JetStream; the dashboard performs the same cash, position and open-order comparisons as the C++ Reconciler defaults without publishing any trading command."
	if !fresh {
		note += " The retained exchange event predates the latest PostgreSQL runtime snapshot, so differences are shown as PENDING rather than treated as a current blocking verdict."
	}

	lastChecked := message.Time
	if parsed, err := time.Parse(time.RFC3339Nano, message.Time); err == nil {
		lastChecked = parsed.UTC().Format("2006-01-02 15:04:05 UTC")
	}

	return realReconciliationData{
		Status:                    status,
		LastChecked:               lastChecked,
		Tolerance:                 "cash 1e-6 / quantity 1e-10 (runtime Reconciler defaults)",
		TargetPortfolioValue:      "Not available",
		LocalPortfolioValue:       "Cash " + formatUSD(state.Snapshot.AccountCash) + " + positions unvalued",
		ExchangePortfolioValue:    "Cash " + formatUSD(event.Snapshot.Cash) + " + positions unvalued",
		Rows:                      buildReconciliationRows(state.Snapshot, &event.Snapshot, true, fresh),
		OpenOrders:                reconciliationOpenOrders(state.Snapshot.Orders),
		SourceMode:                "REAL",
		SourceUpdatedAt:           state.UpdatedAt,
		SourceNote:                note,
		ComparisonAvailable:       true,
		ExchangeEvidenceTime:      message.Time,
		ExchangeEvidenceSequence:  message.Sequence,
		ExchangeSnapshotTimestamp: event.Snapshot.Timestamp,
		EvidenceFresh:             fresh,
		Issues:                    comparison.Issues,
	}
}

func exchangeEvidenceFresh(runtimeUpdatedAt, exchangeMessageTime string) bool {
	runtimeTime, err := time.Parse(time.RFC3339Nano, runtimeUpdatedAt)
	if err != nil {
		return false
	}
	exchangeTime, err := time.Parse(time.RFC3339Nano, exchangeMessageTime)
	if err != nil {
		return false
	}
	// A small clock/commit allowance avoids classifying one transaction boundary as stale.
	return !exchangeTime.Add(2 * time.Second).Before(runtimeTime)
}

func buildReconciliationRows(local tradingwire.TradingStateSnapshot, exchange *tradingwire.ExchangeSnapshot, exchangeAvailable, evidenceFresh bool) []realReconciliationRow {
	pending := pendingSignedQuantities(local.Orders)
	coins := map[string]struct{}{}
	for coin := range local.AccountPositions {
		coins[coin] = struct{}{}
	}
	for coin := range pending {
		coins[coin] = struct{}{}
	}
	if exchange != nil {
		for coin := range exchange.Positions {
			coins[coin] = struct{}{}
		}
	}

	ordered := make([]string, 0, len(coins))
	for coin := range coins {
		ordered = append(ordered, coin)
	}
	sort.Strings(ordered)

	rows := make([]realReconciliationRow, 0, len(ordered))
	for _, coin := range ordered {
		localQty := local.AccountPositions[coin]
		pendingQty := pending[coin]
		effective := localQty + pendingQty
		row := realReconciliationRow{
			Asset:             coin,
			TargetQtyLabel:    "Not available",
			TargetAvailable:   false,
			EffectiveQty:      effective,
			EffectiveQtyLabel: formatQuantity(effective),
			LocalQty:          localQty,
			LocalQtyLabel:     formatQuantity(localQty),
			PendingQty:        pendingQty,
			PendingQtyLabel:   signedQuantity(pendingQty),
			Status:            "UNKNOWN",
			Explanation:       "Exchange evidence is unavailable; no alignment verdict is inferred.",
		}
		if exchangeAvailable && exchange != nil {
			exchangeQty := exchange.Positions[coin]
			delta := exchangeQty - localQty
			row.ExchangeQty = exchangeQty
			row.ExchangeQtyLabel = formatQuantity(exchangeQty)
			row.ExchangeAvailable = true
			row.DeltaQty = delta
			row.DeltaQtyLabel = signedQuantity(delta)
			if !evidenceFresh {
				row.Status = "PENDING"
				row.Explanation = "Exchange evidence predates the latest durable local state; current alignment cannot be proven from this retained snapshot."
			} else if sameReconciliationValue(localQty, exchangeQty, reconciliationQuantityTolerance) {
				row.Status = "ALIGNED"
				row.Explanation = "Durable local filled position matches the retained exchange position within the runtime quantity tolerance."
			} else {
				row.Status = "BLOCKED"
				row.Explanation = "Local filled position differs from current exchange evidence beyond the runtime quantity tolerance."
			}
		} else {
			row.ExchangeQtyLabel = "Not available"
			row.DeltaQtyLabel = "—"
		}
		rows = append(rows, row)
	}
	return rows
}

func reconciliationOpenOrders(orders []tradingwire.PersistedTrackedOrder) []realOpenOrderSummary {
	result := make([]realOpenOrderSummary, 0)
	for _, order := range orders {
		if isTerminalPersistedStatus(order.Status) {
			continue
		}
		state := mapExecutionState(order)
		result = append(result, realOpenOrderSummary{
			OrderID:    strconv.FormatUint(order.OrderID, 10),
			Asset:      order.Coin,
			Side:       sideLabel(order.Side),
			Quantity:   order.Quantity,
			Remaining:  math.Max(0, order.Quantity-order.FilledQuantity),
			Age:        formatTradingTimestamp(order.UpdatedAt),
			State:      state,
			ExchangeID: emptyAsDash(order.ExchangeOrderID),
		})
	}
	sort.Slice(result, func(i, j int) bool {
		left, _ := strconv.ParseUint(result[i].OrderID, 10, 64)
		right, _ := strconv.ParseUint(result[j].OrderID, 10, 64)
		return left > right
	})
	return result
}

func pendingSignedQuantities(orders []tradingwire.PersistedTrackedOrder) map[string]float64 {
	result := make(map[string]float64)
	for _, order := range orders {
		if isTerminalPersistedStatus(order.Status) {
			continue
		}
		remaining := math.Max(0, order.Quantity-order.FilledQuantity)
		if order.Side == 1 {
			remaining = -remaining
		}
		result[order.Coin] += remaining
	}
	return result
}

func compareRuntimeAndExchange(local tradingwire.TradingStateSnapshot, exchange tradingwire.ExchangeSnapshot) reconciliationComparison {
	result := reconciliationComparison{Issues: []realReconciliationIssue{}}
	if !sameReconciliationValue(local.AccountCash, exchange.Cash, reconciliationCashTolerance) {
		result.Issues = append(result.Issues, realReconciliationIssue{Kind: "CashMismatch", LocalValue: local.AccountCash, ExchangeValue: exchange.Cash, Message: "Local cash does not match exchange cash"})
	}

	coins := map[string]struct{}{}
	for coin := range local.AccountPositions {
		coins[coin] = struct{}{}
	}
	for coin := range exchange.Positions {
		coins[coin] = struct{}{}
	}
	orderedCoins := make([]string, 0, len(coins))
	for coin := range coins {
		orderedCoins = append(orderedCoins, coin)
	}
	sort.Strings(orderedCoins)
	for _, coin := range orderedCoins {
		localQty := local.AccountPositions[coin]
		exchangeQty := exchange.Positions[coin]
		if !sameReconciliationValue(localQty, exchangeQty, reconciliationQuantityTolerance) {
			result.Issues = append(result.Issues, realReconciliationIssue{Kind: "PositionMismatch", Asset: coin, LocalValue: localQty, ExchangeValue: exchangeQty, Message: "Local filled position does not match exchange position"})
		}
	}

	matched := make(map[int]struct{})
	for _, localOrder := range local.Orders {
		if isTerminalPersistedStatus(localOrder.Status) {
			continue
		}
		matchIndex := -1
		for i, exchangeOrder := range exchange.OpenOrders {
			if _, used := matched[i]; used {
				continue
			}
			if exchangeOrder.LocalOrderID != 0 && exchangeOrder.LocalOrderID == localOrder.OrderID {
				matchIndex = i
				break
			}
		}
		if matchIndex < 0 && strings.TrimSpace(localOrder.ExchangeOrderID) != "" {
			for i, exchangeOrder := range exchange.OpenOrders {
				if _, used := matched[i]; used {
					continue
				}
				if exchangeOrder.ExchangeOrderID != "" && exchangeOrder.ExchangeOrderID == localOrder.ExchangeOrderID {
					matchIndex = i
					break
				}
			}
		}

		if matchIndex < 0 {
			// Mirrors C++ Reconciler: Created is durably staged before submit and may
			// legitimately be absent from exchange during recovery.
			if localOrder.Status == 0 {
				continue
			}
			result.Issues = append(result.Issues, realReconciliationIssue{
				Kind: "MissingExchangeOrder", Asset: localOrder.Coin, OrderID: strconv.FormatUint(localOrder.OrderID, 10),
				LocalValue: math.Max(0, localOrder.Quantity-localOrder.FilledQuantity), ExchangeValue: 0,
				Message: "Locally open order is not present in exchange open orders",
			})
			continue
		}

		matched[matchIndex] = struct{}{}
		exchangeOrder := exchange.OpenOrders[matchIndex]
		metadataMatches := exchangeOrder.Coin == localOrder.Coin && exchangeOrder.Side == localOrder.Side
		quantityMatches := sameReconciliationValue(exchangeOrder.Quantity, localOrder.Quantity, reconciliationQuantityTolerance) && sameReconciliationValue(exchangeOrder.FilledQuantity, localOrder.FilledQuantity, reconciliationQuantityTolerance)
		if !metadataMatches || !quantityMatches {
			result.Issues = append(result.Issues, realReconciliationIssue{
				Kind: "OrderMismatch", Asset: localOrder.Coin, OrderID: strconv.FormatUint(localOrder.OrderID, 10),
				LocalValue: math.Max(0, localOrder.Quantity-localOrder.FilledQuantity), ExchangeValue: math.Max(0, exchangeOrder.Quantity-exchangeOrder.FilledQuantity),
				Message: "Local and exchange order details differ",
			})
		}
	}

	for i, exchangeOrder := range exchange.OpenOrders {
		if _, ok := matched[i]; ok {
			continue
		}
		result.Issues = append(result.Issues, realReconciliationIssue{
			Kind: "UnexpectedExchangeOrder", Asset: exchangeOrder.Coin, OrderID: strconv.FormatUint(exchangeOrder.LocalOrderID, 10),
			LocalValue: 0, ExchangeValue: math.Max(0, exchangeOrder.Quantity-exchangeOrder.FilledQuantity),
			Message: "Exchange has an open order not represented by local open-order state",
		})
	}

	return result
}

func sameReconciliationValue(a, b, tolerance float64) bool {
	return !math.IsNaN(a) && !math.IsInf(a, 0) && !math.IsNaN(b) && !math.IsInf(b, 0) && math.Abs(a-b) <= tolerance
}

func isTerminalPersistedStatus(status int) bool {
	return status == 4 || status == 5 || status == 6
}

func emptyAsDash(value string) string {
	if strings.TrimSpace(value) == "" {
		return "—"
	}
	return value
}
