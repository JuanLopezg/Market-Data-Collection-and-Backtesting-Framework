package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

const maxExecutionOrders = 200
const maxExecutionFills = 1000

type realExecutionData struct {
	LastUpdated           string                  `json:"lastUpdated"`
	OpenOrders            int                     `json:"openOrders"`
	PartialOrders         int                     `json:"partialOrders"`
	PendingCancels        int                     `json:"pendingCancels"`
	FilledOrders          int                     `json:"filledOrders"`
	FillCount             int64                   `json:"fillCount"`
	RejectCount           int                     `json:"rejectCount"`
	AvgSubmitLatencyMs    int                     `json:"avgSubmitLatencyMs"`
	AvgFillLatencyMs      int                     `json:"avgFillLatencyMs"`
	AvgSlippageBpsLabel   string                  `json:"avgSlippageBpsLabel"`
	SubmitLatencyP95Ms    int                     `json:"submitLatencyP95Ms"`
	FillLatencyP95Ms      int                     `json:"fillLatencyP95Ms"`
	BestSlippageBpsLabel  string                  `json:"bestSlippageBpsLabel"`
	WorstSlippageBpsLabel string                  `json:"worstSlippageBpsLabel"`
	TotalFeesLabel        string                  `json:"totalFeesLabel"`
	RejectRateLabel       string                  `json:"rejectRateLabel"`
	Orders                []realExecutionOrder    `json:"orders"`
	PartialFills          []realExecutionFill     `json:"partialFills"`
	Rejects               []realExecutionReject   `json:"rejects"`
	Replacements          []realCancelReplacement `json:"replacements"`
	SourceMode            string                  `json:"sourceMode"`
	SourceUpdatedAt       string                  `json:"sourceUpdatedAt"`
	SourceNote            string                  `json:"sourceNote"`
	LatencyAvailable      bool                    `json:"latencyAvailable"`
	SlippageAvailable     bool                    `json:"slippageAvailable"`
	OrderWindowTruncated  bool                    `json:"orderWindowTruncated"`
	FillWindowTruncated   bool                    `json:"fillWindowTruncated"`
}

type realExecutionOrder struct {
	OrderID            string                   `json:"orderId"`
	ExchangeOrderID    string                   `json:"exchangeOrderId"`
	CycleID            string                   `json:"cycleId"`
	CorrelationID      string                   `json:"correlationId"`
	StrategyID         uint64                   `json:"strategyId"`
	Asset              string                   `json:"asset"`
	Side               string                   `json:"side"`
	Quantity           float64                  `json:"quantity"`
	FilledQty          float64                  `json:"filledQty"`
	RemainingQty       float64                  `json:"remainingQty"`
	State              string                   `json:"state"`
	Age                string                   `json:"age"`
	ExpectedPriceLabel string                   `json:"expectedPriceLabel"`
	AvgFillPriceLabel  string                   `json:"avgFillPriceLabel"`
	FeesLabel          string                   `json:"feesLabel"`
	SlippageBps        float64                  `json:"slippageBps"`
	SlippageBpsLabel   string                   `json:"slippageBpsLabel"`
	SubmitLatencyMs    int                      `json:"submitLatencyMs"`
	FillLatencyMs      *int                     `json:"fillLatencyMs"`
	SubmittedAt        string                   `json:"submittedAt"`
	LastUpdateAt       string                   `json:"lastUpdateAt"`
	Lifecycle          []realExecutionLifecycle `json:"lifecycle"`
}

type realExecutionLifecycle struct {
	Time   string `json:"time"`
	State  string `json:"state"`
	Detail string `json:"detail"`
	Tone   string `json:"tone"`
}

type realExecutionFill struct {
	FillID          string  `json:"fillId"`
	OrderID         string  `json:"orderId"`
	Asset           string  `json:"asset"`
	Quantity        float64 `json:"quantity"`
	PriceLabel      string  `json:"priceLabel"`
	CumulativeLabel string  `json:"cumulativeLabel"`
	FeesLabel       string  `json:"feesLabel"`
	Timestamp       string  `json:"timestamp"`
}

type realExecutionReject struct {
	OrderID     string `json:"orderId"`
	Asset       string `json:"asset"`
	Code        string `json:"code"`
	Reason      string `json:"reason"`
	Remediation string `json:"remediation"`
}

type realCancelReplacement struct {
	Asset              string `json:"asset"`
	OriginalOrderID    string `json:"originalOrderId"`
	CancelRequestedAt  string `json:"cancelRequestedAt"`
	CanceledAt         string `json:"canceledAt"`
	ReplacementOrderID string `json:"replacementOrderId"`
	ReplacementState   string `json:"replacementState"`
}

type orderFillAggregate struct {
	Quantity   float64
	Notional   float64
	Commission float64
}

func (p *Real) execution(ctx context.Context) (realExecutionData, error) {
	state, err := p.postgres.RuntimeState(ctx)
	if err != nil {
		return realExecutionData{}, fmt.Errorf("read durable execution state: %w", err)
	}

	tracked := append([]tradingwire.PersistedTrackedOrder(nil), state.Snapshot.Orders...)
	sort.Slice(tracked, func(i, j int) bool {
		if tracked[i].UpdatedAt == tracked[j].UpdatedAt {
			return tracked[i].OrderID > tracked[j].OrderID
		}
		return tracked[i].UpdatedAt > tracked[j].UpdatedAt
	})
	totalOrders := len(tracked)
	if len(tracked) > maxExecutionOrders {
		tracked = tracked[:maxExecutionOrders]
	}

	ids := make([]uint64, 0, len(tracked))
	for _, order := range tracked {
		ids = append(ids, order.OrderID)
	}
	fills, err := p.postgres.FillsForOrders(ctx, ids, maxExecutionFills)
	if err != nil {
		return realExecutionData{}, fmt.Errorf("read durable execution fills: %w", err)
	}

	return buildRealExecution(state, tracked, totalOrders, fills), nil
}

func buildRealExecution(state pgstore.RuntimeState, tracked []tradingwire.PersistedTrackedOrder, totalOrders int, fills pgstore.FillWindow) realExecutionData {
	fillsTruncated := len(fills.Rows) >= maxExecutionFills
	fillsByOrder := make(map[uint64]orderFillAggregate)
	cumulative := make(map[uint64]float64)
	fillRows := make([]realExecutionFill, 0, len(fills.Rows))
	for _, fill := range fills.Rows {
		agg := fillsByOrder[fill.OrderID]
		agg.Quantity += fill.Quantity
		agg.Notional += fill.Quantity * fill.Price
		agg.Commission += fill.Commission
		fillsByOrder[fill.OrderID] = agg

		cumulative[fill.OrderID] += fill.Quantity
		cumulativeLabel := formatQuantity(cumulative[fill.OrderID])
		if fillsTruncated {
			cumulativeLabel = "Bounded window"
		}
		fillRows = append(fillRows, realExecutionFill{
			FillID:          strconv.FormatUint(fill.FillID, 10),
			OrderID:         strconv.FormatUint(fill.OrderID, 10),
			Asset:           fill.Coin,
			Quantity:        fill.Quantity,
			PriceLabel:      formatPriceUSD(fill.Price),
			CumulativeLabel: cumulativeLabel,
			FeesLabel:       formatUSD(fill.Commission),
			Timestamp:       formatTradingTimestamp(fill.Timestamp),
		})
	}
	// Present newest persisted fills first in the table.
	for left, right := 0, len(fillRows)-1; left < right; left, right = left+1, right-1 {
		fillRows[left], fillRows[right] = fillRows[right], fillRows[left]
	}

	orders := make([]realExecutionOrder, 0, len(tracked))
	rejects := make([]realExecutionReject, 0)
	openOrders, partialOrders, pendingCancels, filledOrders := 0, 0, 0, 0

	for _, order := range tracked {
		stateLabel := mapExecutionState(order)
		switch stateLabel {
		case "NEW":
			openOrders++
		case "PARTIAL":
			openOrders++
			partialOrders++
		case "PENDING_CANCEL":
			openOrders++
			pendingCancels++
		case "FILLED":
			filledOrders++
		case "REJECTED":
			reason := strings.TrimSpace(order.LastMessage)
			if reason == "" {
				reason = "Rejected state is durable, but no rejection message is persisted."
			}
			rejects = append(rejects, realExecutionReject{
				OrderID:     strconv.FormatUint(order.OrderID, 10),
				Asset:       order.Coin,
				Code:        "REJECTED",
				Reason:      reason,
				Remediation: "No remediation is inferred by the dashboard; inspect the execution logs/audit trail before retrying.",
			})
		}

		fillAgg := fillsByOrder[order.OrderID]
		avgFill := "—"
		feesLabel := formatUSD(fillAgg.Commission)
		if fillsTruncated {
			avgFill = "Bounded window"
			feesLabel = "Bounded window"
		} else if fillAgg.Quantity > 0 {
			avgFill = formatPriceUSD(fillAgg.Notional / fillAgg.Quantity)
		}
		lifecycle := []realExecutionLifecycle{{
			Time:   formatTradingTimestamp(order.CreatedAt),
			State:  "CREATED",
			Detail: fmt.Sprintf("Durable order %d for strategy %d: %s %s %s.", order.OrderID, order.StrategyID, sideLabel(order.Side), formatQuantity(order.Quantity), order.Coin),
			Tone:   "info",
		}}
		if order.UpdatedAt != order.CreatedAt || stateLabel != "NEW" {
			lifecycle = append(lifecycle, realExecutionLifecycle{
				Time:   formatTradingTimestamp(order.UpdatedAt),
				State:  stateLabel,
				Detail: lifecycleDetail(order, stateLabel),
				Tone:   lifecycleTone(stateLabel),
			})
		}

		remaining := math.Max(0, order.Quantity-order.FilledQuantity)
		orders = append(orders, realExecutionOrder{
			OrderID:            strconv.FormatUint(order.OrderID, 10),
			ExchangeOrderID:    order.ExchangeOrderID,
			CycleID:            "Not persisted",
			CorrelationID:      "Not persisted",
			StrategyID:         order.StrategyID,
			Asset:              order.Coin,
			Side:               sideLabel(order.Side),
			Quantity:           order.Quantity,
			FilledQty:          order.FilledQuantity,
			RemainingQty:       remaining,
			State:              stateLabel,
			Age:                "—",
			ExpectedPriceLabel: "Not persisted",
			AvgFillPriceLabel:  avgFill,
			FeesLabel:          feesLabel,
			SlippageBpsLabel:   "Not available",
			SubmittedAt:        formatTradingTimestamp(order.CreatedAt),
			LastUpdateAt:       formatTradingTimestamp(order.UpdatedAt),
			Lifecycle:          lifecycle,
		})
	}

	rejectRate := "0.0%"
	if len(tracked) > 0 {
		rejectRate = fmt.Sprintf("%.1f%%", 100*float64(len(rejects))/float64(len(tracked)))
	}

	note := "Orders come from trading_runtime_state.snapshot.orders and fills from trading_fills. Price/slippage and gateway latency are not inferred because the durable runtime contract does not persist a canonical expected price or wall-clock latency trace. Replacement lineage is not persisted."
	if totalOrders > maxExecutionOrders {
		note += fmt.Sprintf(" Showing the newest %d of %d tracked orders.", maxExecutionOrders, totalOrders)
	}
	if fillsTruncated {
		note += fmt.Sprintf(" Fill detail reached the bounded %d-row limit; per-order average fill and fee values are suppressed to avoid partial economics.", maxExecutionFills)
	}

	return realExecutionData{
		LastUpdated:           state.UpdatedAt,
		OpenOrders:            openOrders,
		PartialOrders:         partialOrders,
		PendingCancels:        pendingCancels,
		FilledOrders:          filledOrders,
		FillCount:             fills.TotalRows,
		RejectCount:           len(rejects),
		AvgSlippageBpsLabel:   "Not available",
		BestSlippageBpsLabel:  "Not available",
		WorstSlippageBpsLabel: "Not available",
		TotalFeesLabel:        formatUSD(fills.TotalCommission),
		RejectRateLabel:       rejectRate,
		Orders:                orders,
		PartialFills:          fillRows,
		Rejects:               rejects,
		Replacements:          []realCancelReplacement{},
		SourceMode:            "REAL",
		SourceUpdatedAt:       state.UpdatedAt,
		SourceNote:            note,
		LatencyAvailable:      false,
		SlippageAvailable:     false,
		OrderWindowTruncated:  totalOrders > maxExecutionOrders,
		FillWindowTruncated:   fillsTruncated,
	}
}

func mapExecutionState(order tradingwire.PersistedTrackedOrder) string {
	if order.CancelRequested && order.Status >= 0 && order.Status <= 3 {
		return "PENDING_CANCEL"
	}
	switch order.Status {
	case 0, 1, 2:
		return "NEW"
	case 3:
		return "PARTIAL"
	case 4:
		return "FILLED"
	case 5:
		return "CANCELED"
	case 6:
		return "REJECTED"
	default:
		return "NEW"
	}
}

func sideLabel(side int) string {
	if side == 1 {
		return "SELL"
	}
	return "BUY"
}

func lifecycleTone(state string) string {
	switch state {
	case "FILLED":
		return "good"
	case "REJECTED":
		return "bad"
	case "PARTIAL", "PENDING_CANCEL", "NEW":
		return "warn"
	default:
		return "muted"
	}
}

func lifecycleDetail(order tradingwire.PersistedTrackedOrder, state string) string {
	message := strings.TrimSpace(order.LastMessage)
	if message != "" {
		return message
	}
	switch state {
	case "FILLED":
		return fmt.Sprintf("Persisted filled quantity is %s %s.", formatQuantity(order.FilledQuantity), order.Coin)
	case "PARTIAL":
		return fmt.Sprintf("Persisted partial fill is %s of %s %s.", formatQuantity(order.FilledQuantity), formatQuantity(order.Quantity), order.Coin)
	case "PENDING_CANCEL":
		return "Cancel requested flag is persisted; waiting for a terminal exchange update."
	case "CANCELED":
		return "Order is persisted in terminal Canceled state."
	case "REJECTED":
		return "Order is persisted in terminal Rejected state."
	default:
		return "Latest durable execution state from ExecutionState."
	}
}

func formatPriceUSD(value float64) string {
	if value <= 0 || math.IsNaN(value) || math.IsInf(value, 0) {
		return "—"
	}
	return fmt.Sprintf("$%.8f", value)
}

func formatTradingTimestamp(value uint64) string {
	if value == 0 {
		return "—"
	}
	raw := strconv.FormatUint(value, 10)
	if len(raw) == 8 {
		if parsed, err := time.Parse("20060102", raw); err == nil {
			return parsed.Format("2006-01-02")
		}
	}
	return raw
}
