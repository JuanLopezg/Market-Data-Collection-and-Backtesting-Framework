package provider

import (
	"fmt"
	"math"
	"sort"

	pgstore "control-dashboard-api/internal/integration/postgres"
)

// Risk page read model shared by the replay and PAPER providers: headline cards, leverage
// and drawdown history, per-strategy limits and per-asset exposure against its cap.
// Display only; account alerts stay warnings and nothing here limits trading.

type riskCard struct {
	Label string `json:"label"`
	Value string `json:"value"`
	Tone  string `json:"tone,omitempty"` // positive, elevated, negative or empty
}

type riskHistoryPoint struct {
	Label    string   `json:"label"`
	Leverage *float64 `json:"leverage,omitempty"` // gross / equity; nil when not known
	Drawdown float64  `json:"drawdown"`           // % below the running equity peak (<= 0)
}

type riskStrategyLimit struct {
	Name          string `json:"name"`
	Capital       string `json:"capital"`
	Sizer         string `json:"sizer"`
	MaxAsset      string `json:"maxAsset"`
	MaxGross      string `json:"maxGross"`
	SizedGross    string `json:"sizedGross"` // gross after the asset caps, before the gross scale
	Scale         string `json:"scale"`
	ApprovedGross string `json:"approvedGross"`
	Binding       string `json:"binding"` // None, Asset cap, Gross cap
}

type riskExposure struct {
	Asset       string  `json:"asset"`
	Side        string  `json:"side"`
	Notional    string  `json:"notional"`
	Weight      float64 `json:"weight"` // % of equity, signed
	WeightLabel string  `json:"weightLabel"`
	Approved    string  `json:"approved"`    // approved weight, % of equity
	Cap         string  `json:"cap"`         // per-asset cap, % of capital
	Utilization float64 `json:"utilization"` // |weight| / cap, %; 0 when the cap is unknown
}

// Inputs of the risk view.
type riskPositionValue struct {
	asset    string
	value    float64  // signed position value (USD)
	approved *float64 // approved weight, fraction of equity
}

type riskEquityPoint struct {
	label  string
	equity float64
	gross  *float64
}

// Leverage tone from the user's scale: green up to 1.5x, orange up to 2.5x, red above.
func leverageTone(value float64) string {
	switch {
	case value <= 1.5:
		return "positive"
	case value <= 2.5:
		return "elevated"
	}
	return "negative"
}

func drawdownTone(pct float64) string {
	switch {
	case pct > -10:
		return ""
	case pct > -25:
		return "elevated"
	}
	return "negative"
}

// Drawdown series: % below the running equity peak, plus leverage when gross is known.
func riskHistory(points []riskEquityPoint) ([]riskHistoryPoint, float64) {
	history := make([]riskHistoryPoint, 0, len(points))
	peak, worst := 0.0, 0.0
	for _, point := range points {
		if point.equity <= 0 {
			continue
		}
		peak = math.Max(peak, point.equity)
		drawdown := (point.equity - peak) / peak * 100
		worst = math.Min(worst, drawdown)
		row := riskHistoryPoint{Label: point.label, Drawdown: drawdown}
		if point.gross != nil {
			leverage := *point.gross / point.equity
			row.Leverage = &leverage
		}
		history = append(history, row)
	}
	return history, worst
}

// Cards and exposure rows from the current account. assetCap is a fraction (0 = unknown).
func riskAccountView(equity float64, positions []riskPositionValue, assetCap float64, history []riskHistoryPoint, maxDrawdown float64) ([]riskCard, []riskExposure) {
	gross, net, largest, largestAsset := 0.0, 0.0, 0.0, "—"
	for _, position := range positions {
		gross += math.Abs(position.value)
		net += position.value
		if math.Abs(position.value) > largest {
			largest, largestAsset = math.Abs(position.value), position.asset
		}
	}
	cards := []riskCard{}
	exposures := []riskExposure{}
	if equity <= 0 {
		return []riskCard{{Label: "Leverage", Value: "Not valued"}}, exposures
	}
	leverage := gross / equity
	largestLabel := "—"
	if largest > 0 {
		largestLabel = largestAsset + " " + formatPercent(largest/equity*100)
	}
	cards = append(cards,
		riskCard{Label: "Leverage", Value: fmt.Sprintf("%.2fx", leverage), Tone: leverageTone(leverage)},
		riskCard{Label: "Gross Exposure", Value: formatPercent(gross / equity * 100)},
		riskCard{Label: "Net Exposure", Value: formatPercent(net / equity * 100)},
		riskCard{Label: "Largest Position", Value: largestLabel})
	if len(history) > 0 {
		current := history[len(history)-1].Drawdown
		cards = append(cards,
			riskCard{Label: "Drawdown", Value: formatPercent(current), Tone: drawdownTone(current)},
			riskCard{Label: "Max Drawdown", Value: formatPercent(maxDrawdown), Tone: drawdownTone(maxDrawdown)})
	}

	for _, position := range positions {
		weight := position.value / equity * 100
		side := "Long"
		if position.value < 0 {
			side = "Short"
		}
		row := riskExposure{Asset: position.asset, Side: side, Notional: formatDollars(math.Abs(position.value)),
			Weight: weight, WeightLabel: formatPercent(weight), Approved: "—", Cap: "—"}
		if position.approved != nil {
			row.Approved = formatPercent(*position.approved * 100)
		}
		if assetCap > 0 {
			row.Cap = formatPercent(assetCap * 100)
			row.Utilization = math.Abs(weight) / (assetCap * 100) * 100
		}
		exposures = append(exposures, row)
	}
	sort.Slice(exposures, func(i, j int) bool { return math.Abs(exposures[i].Weight) > math.Abs(exposures[j].Weight) })
	return cards, exposures
}

// Which limit changed the sized weights: the gross scale, an asset cap, or none.
func riskBinding(scale float64, assetCapReduced bool) string {
	switch {
	case scale < 1-1e-9:
		return "Gross cap"
	case assetCapReduced:
		return "Asset cap"
	}
	return "None"
}

// flowAdjustedEquity turns the daily account rows into a performance index for the drawdown,
// so deposits and withdrawals are not gains or losses (user request 2026-10-22). Each day's
// return is the change of cash + positions minus that day's USD capital flows, over the
// previous day's whole capital (cash + positions + BTC collateral after haircut). The BTC
// collateral is capital, not strategy performance: a BTC deposit or a BTC price move changes
// the base, never the return. The index starts at the first day's whole capital.
func flowAdjustedEquity(days []pgstore.EquityDay) []riskEquityPoint {
	if len(days) == 0 {
		return nil
	}
	index := days[0].Equity + days[0].CollateralUSD
	points := []riskEquityPoint{{label: formatTradingTimestamp(days[0].Date), equity: index}}
	for i := 1; i < len(days); i++ {
		previous, day := days[i-1], days[i]
		base := previous.Equity + previous.CollateralUSD
		pnl := (day.Equity - previous.Equity) - (day.CapitalFlows - previous.CapitalFlows)
		if base > 0 && index > 0 {
			index *= 1 + pnl/base
		}
		points = append(points, riskEquityPoint{label: formatTradingTimestamp(day.Date), equity: index})
	}
	return points
}
