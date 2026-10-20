package provider

import (
	"math"
	"sort"
	"strconv"
	"strings"
	"time"

	"control-dashboard-api/internal/tradingwire"
)

func executionReferencePrice(order tradingwire.PersistedTrackedOrder) (float64, string) {
	if finitePositive(order.PositionStopPrice) {
		return order.PositionStopPrice, "Stop trigger (USD per source unit)"
	}
	if finitePositive(order.ReferencePrice) {
		return order.ReferencePrice, "Persisted planning price (USD per source unit)"
	}
	return 0, "No reference price stored for this order"
}

// Keep small real-money fees visible instead of rounding them to $0.00.
func executionFeesLabel(value float64, pending bool) string {
	if pending && value == 0 {
		return "Pending"
	}
	label := strings.TrimRight(strconv.FormatFloat(value, 'f', 8, 64), "0")
	if strings.HasSuffix(label, ".") {
		label += "00"
	} else if len(label)-strings.IndexByte(label, '.') == 2 {
		label += "0"
	}
	label = "$" + label
	if pending {
		label += " + pending"
	}
	return label
}

type executionSlippageDay struct {
	Label string  `json:"label"`
	Bps   float64 `json:"bps"`
	Fills int     `json:"fills"`
}

type executionSlippageSummary struct {
	averageLabel, bestLabel, worstLabel string
	fills                               int
	daily                               []executionSlippageDay
}

// Each fill is measured in source units against its own immutable order reference.
// Sum daily bps; weight the overall average by actual filled USD notional.
func realExecutionSlippage(orders []tradingwire.PersistedTrackedOrder, fills []tradingwire.Fill) executionSlippageSummary {
	byID := make(map[uint64]tradingwire.PersistedTrackedOrder, len(orders))
	for _, order := range orders {
		byID[order.OrderID] = order
	}
	type day struct {
		bps   float64
		fills int
	}
	days := map[string]*day{}
	totalWeighted, totalNotional := 0.0, 0.0
	best, worst := math.Inf(1), math.Inf(-1)
	result := executionSlippageSummary{averageLabel: "Not available", bestLabel: "Not available", worstLabel: "Not available", daily: []executionSlippageDay{}}
	for _, fill := range fills {
		order, exists := byID[fill.OrderID]
		if !exists || order.Coin != fill.Coin || order.Side != fill.Side || !finitePositive(fill.Quantity) {
			continue
		}
		expected, _ := executionReferencePrice(order)
		bps, valid := slippageBpsFor(sideLabel(order.Side), expected, fill.Price)
		notional := fill.Quantity * fill.Price
		label := executionFillDay(fill.Timestamp)
		if !valid || !finitePositive(notional) || !isFiniteLedger(bps*notional) || label == "" {
			continue
		}
		if days[label] == nil {
			days[label] = &day{}
		}
		d := days[label]
		d.bps += bps
		d.fills++
		totalWeighted += bps * notional
		totalNotional += notional
		result.fills++
		best, worst = math.Min(best, bps), math.Max(worst, bps)
	}
	if totalNotional == 0 {
		return result
	}
	result.averageLabel, result.bestLabel, result.worstLabel = formatBps(totalWeighted/totalNotional), formatBps(best), formatBps(worst)
	labels := make([]string, 0, len(days))
	for label := range days {
		labels = append(labels, label)
	}
	sort.Strings(labels)
	for _, label := range labels {
		d := days[label]
		result.daily = append(result.daily, executionSlippageDay{Label: label, Bps: d.bps, Fills: d.fills})
	}
	return result
}

func executionFillDay(timestamp uint64) string {
	raw := strconv.FormatUint(timestamp, 10)
	if len(raw) == 9 && (raw[8] == '1' || raw[8] == '9') {
		raw = raw[:8]
	}
	if len(raw) == 8 {
		if parsed, err := time.Parse("20060102", raw); err == nil {
			return parsed.Format("2006-01-02")
		}
	}
	return ""
}
