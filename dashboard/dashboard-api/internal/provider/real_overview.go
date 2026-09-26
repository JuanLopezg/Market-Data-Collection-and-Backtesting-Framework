package provider

import (
	"context"
	"fmt"
	"strings"
)

type realOverviewData struct {
	Stats                 []realOverviewMetric `json:"stats"`
	EquityCurve           []realEquityPoint    `json:"equityCurve"`
	LiveExpected          []realEquityPoint    `json:"liveExpected"`
	Positions             []realPosition       `json:"positions"`
	RecentEvents          []realRecentEvent    `json:"recentEvents"`
	Checks                []realOverviewCheck  `json:"checks"`
	SourceMode            string               `json:"sourceMode"`
	SourceUpdatedAt       string               `json:"sourceUpdatedAt"`
	SourceNote            string               `json:"sourceNote"`
	Readiness             string               `json:"readiness"`
	ReadinessDetail       string               `json:"readinessDetail"`
	ReconciliationStatus  string               `json:"reconciliationStatus"`
	EquityAvailable       bool                 `json:"equityAvailable"`
	LiveExpectedAvailable bool                 `json:"liveExpectedAvailable"`
	MarketDataStatus      string               `json:"marketDataStatus"`
	CanonicalCycle        string               `json:"canonicalCycle"`
	SourceWarnings        []string             `json:"sourceWarnings"`
}

type realOverviewMetric struct {
	Label  string    `json:"label"`
	Value  string    `json:"value"`
	Delta  string    `json:"delta,omitempty"`
	Detail string    `json:"detail,omitempty"`
	Trend  []float64 `json:"trend,omitempty"`
}

type realEquityPoint struct {
	Label     string  `json:"label"`
	Equity    float64 `json:"equity"`
	Benchmark float64 `json:"benchmark"`
}

type realRecentEvent struct {
	Time     string `json:"time"`
	Event    string `json:"event"`
	Source   string `json:"source"`
	Severity string `json:"severity"`
}

type realOverviewCheck struct {
	Label  string `json:"label"`
	State  string `json:"state"`
	Detail string `json:"detail"`
}

func (p *Real) overview(ctx context.Context) (realOverviewData, error) {
	// The durable runtime snapshot is the minimum safe boundary for Overview.
	// If it is unavailable, the page fails closed instead of mixing stale fixture data.
	positions, err := p.positions(ctx)
	if err != nil {
		return realOverviewData{}, fmt.Errorf("read overview durable account state: %w", err)
	}

	var warnings []string

	reconciliation, reconciliationErr := p.reconciliation(ctx)
	if reconciliationErr != nil {
		warnings = append(warnings, "Reconciliation read model unavailable: "+reconciliationErr.Error())
	}

	execution, executionErr := p.execution(ctx)
	if executionErr != nil {
		warnings = append(warnings, "Execution read model unavailable: "+executionErr.Error())
	}

	risk, riskErr := p.risk(ctx)
	if riskErr != nil {
		warnings = append(warnings, "Risk decision checkpoint unavailable: "+riskErr.Error())
	}

	market, marketErr := p.marketDataResource(ctx)
	if marketErr != nil {
		warnings = append(warnings, "Market-data read model unavailable: "+marketErr.Error())
	}

	return buildRealOverview(positions, reconciliation, reconciliationErr, execution, executionErr, risk, riskErr, market, marketErr, warnings), nil
}

func buildRealOverview(
	positions realPositionsData,
	reconciliation realReconciliationData,
	reconciliationErr error,
	execution realExecutionData,
	executionErr error,
	risk realRiskData,
	riskErr error,
	market realMarketData,
	marketErr error,
	warnings []string,
) realOverviewData {
	totalEquity := realOverviewMetric{
		Label:  "Total Equity",
		Value:  "Not available",
		Delta:  "—",
		Detail: "Canonical ledger / mark-to-market not wired yet",
	}
	cash := realOverviewMetric{
		Label:  "Cash",
		Value:  positions.AccountCash,
		Delta:  "REAL",
		Detail: "Durable ExecutionState snapshot",
	}
	approvedTarget := realOverviewMetric{
		Label:  "Approved Gross Target",
		Value:  "Not available",
		Delta:  "—",
		Detail: "PortfolioRisk checkpoint unavailable",
	}
	if riskErr == nil {
		approvedTarget.Value = risk.ApprovedGrossTargetLabel
		approvedTarget.Delta = risk.ApprovedTargetNotionalLabel
		approvedTarget.Detail = fmt.Sprintf("%d active approved target(s)", risk.ActiveTargetCount)
	}

	reconciliationStatus := "PENDING"
	if reconciliationErr == nil && strings.TrimSpace(reconciliation.Status) != "" {
		reconciliationStatus = reconciliation.Status
	}

	marketState := "UNKNOWN"
	canonicalCycle := "—"
	if marketErr == nil {
		canonicalCycle = market.LatestCompletedCandle
		marketState = overviewMarketState(market)
	}

	readiness := "DEGRADED"
	readinessDetail := "Partial real readiness only; global shell readiness, exchange connectivity and alerts are intentionally not aggregated until Step 27."
	if reconciliationStatus == "BLOCKED" {
		readiness = "PAUSED"
		readinessDetail = "Current reconciliation evidence is BLOCKED. Overview does not promote the system to ready."
	} else if len(warnings) > 0 {
		readinessDetail = "One or more real overview sources are unavailable; the page is showing only verified data that could be read safely."
	}

	checks := []realOverviewCheck{
		{
			Label:  "Reconciliation",
			State:  overviewReconciliationCheckState(reconciliationStatus),
			Detail: overviewReconciliationDetail(reconciliation, reconciliationErr),
		},
		{
			Label:  "Market cycle",
			State:  marketState,
			Detail: overviewMarketDetail(market, marketErr),
		},
		{
			Label:  "Durable runtime",
			State:  "ALIGNED",
			Detail: "Account cash and physical positions were decoded from trading_runtime_state.snapshot.",
		},
	}

	events := buildOverviewEvents(reconciliation, reconciliationErr, execution, executionErr, risk, riskErr, market, marketErr)

	topPositions := positions.Positions
	if len(topPositions) > 8 {
		topPositions = append([]realPosition(nil), topPositions[:8]...)
	}

	note := "Step 23 Overview is a read-only aggregate over the already verified real read models: trading_runtime_state, PortfolioRisk checkpoints, trading_fills, retained reconciliation evidence and canonical market-data SQLite. Total equity, realized/unrealized PnL, an equity curve and Live-vs-Expected are intentionally not fabricated because the project does not yet expose a canonical ledger/mark-to-market/baseline read model."
	if len(warnings) > 0 {
		note += " Some components are degraded; see sourceWarnings."
	}

	return realOverviewData{
		Stats:                 []realOverviewMetric{totalEquity, cash, approvedTarget},
		EquityCurve:           []realEquityPoint{},
		LiveExpected:          []realEquityPoint{},
		Positions:             topPositions,
		RecentEvents:          events,
		Checks:                checks,
		SourceMode:            "REAL",
		SourceUpdatedAt:       positions.SourceUpdatedAt,
		SourceNote:            note,
		Readiness:             readiness,
		ReadinessDetail:       readinessDetail,
		ReconciliationStatus:  reconciliationStatus,
		EquityAvailable:       false,
		LiveExpectedAvailable: false,
		MarketDataStatus:      marketState,
		CanonicalCycle:        canonicalCycle,
		SourceWarnings:        append([]string(nil), warnings...),
	}
}

func overviewMarketState(market realMarketData) string {
	if !market.SignalCycleAligned || market.StaleAssets > 0 || market.Integrity.InvalidRows > 0 || market.Integrity.DuplicateTimestamps > 0 {
		return "PENDING"
	}
	return "ALIGNED"
}

func overviewMarketDetail(market realMarketData, err error) string {
	if err != nil {
		return "Canonical market-data status unavailable: " + err.Error()
	}
	if !market.SignalCycleAligned {
		return "Canonical SQLite is readable, but the durable Strategy checkpoint is not aligned to the latest market cycle."
	}
	if market.StaleAssets > 0 {
		return fmt.Sprintf("%d of %d strategy-universe assets are stale at the canonical frontier.", market.StaleAssets, market.TotalAssets)
	}
	if market.Integrity.InvalidRows > 0 || market.Integrity.DuplicateTimestamps > 0 {
		return fmt.Sprintf("Integrity diagnostics: %d invalid rows, %d duplicate timestamps.", market.Integrity.InvalidRows, market.Integrity.DuplicateTimestamps)
	}
	return fmt.Sprintf("Canonical cycle %s; %d/%d strategy-universe assets current.", market.LatestCompletedCandle, market.HealthyAssets, market.TotalAssets)
}

func overviewReconciliationCheckState(status string) string {
	switch status {
	case "CLEAN":
		return "ALIGNED"
	case "BLOCKED":
		return "BLOCKED"
	case "DRIFT":
		return "DRIFT"
	default:
		return "PENDING"
	}
}

func overviewReconciliationDetail(reconciliation realReconciliationData, err error) string {
	if err != nil {
		return "Reconciliation read model unavailable: " + err.Error()
	}
	if strings.TrimSpace(reconciliation.SourceNote) != "" {
		return reconciliation.SourceNote
	}
	return "No reconciliation detail is available."
}

func buildOverviewEvents(
	reconciliation realReconciliationData,
	reconciliationErr error,
	execution realExecutionData,
	executionErr error,
	risk realRiskData,
	riskErr error,
	market realMarketData,
	marketErr error,
) []realRecentEvent {
	events := make([]realRecentEvent, 0, 8)

	if executionErr == nil {
		for _, reject := range execution.Rejects {
			if len(events) >= 2 {
				break
			}
			events = append(events, realRecentEvent{
				Time:     execution.SourceUpdatedAt,
				Event:    fmt.Sprintf("Order %s %s rejected: %s", reject.OrderID, reject.Asset, reject.Reason),
				Source:   "Execution",
				Severity: "WARN",
			})
		}
		for _, fill := range execution.PartialFills {
			if len(events) >= 5 {
				break
			}
			events = append(events, realRecentEvent{
				Time:     fill.Timestamp,
				Event:    fmt.Sprintf("Persisted fill %s %s qty %s @ %s", fill.FillID, fill.Asset, formatQuantity(fill.Quantity), fill.PriceLabel),
				Source:   "Execution",
				Severity: "INFO",
			})
		}
	}

	if reconciliationErr == nil {
		severity := "INFO"
		if reconciliation.Status == "PENDING" || reconciliation.Status == "DRIFT" {
			severity = "WARN"
		} else if reconciliation.Status == "BLOCKED" {
			severity = "CRITICAL"
		}
		events = append(events, realRecentEvent{
			Time:     reconciliation.LastChecked,
			Event:    "Reconciliation evidence state: " + reconciliation.Status,
			Source:   "Reconciliation",
			Severity: severity,
		})
	}

	if riskErr == nil {
		events = append(events, realRecentEvent{
			Time:     risk.DecisionTimestamp,
			Event:    fmt.Sprintf("PortfolioRisk approved %d active target(s), gross %s", risk.ActiveTargetCount, risk.ApprovedGrossTargetLabel),
			Source:   "Risk",
			Severity: "INFO",
		})
	}

	if marketErr == nil {
		severity := "INFO"
		if overviewMarketState(market) != "ALIGNED" {
			severity = "WARN"
		}
		events = append(events, realRecentEvent{
			Time:     market.LatestCompletedCandle,
			Event:    fmt.Sprintf("Canonical market cycle %s: %d active signal(s), %d stale asset(s)", market.LatestCompletedCandle, market.ActiveSignals, market.StaleAssets),
			Source:   "Market Data",
			Severity: severity,
		})
	}

	if len(events) > 8 {
		events = events[:8]
	}
	if len(events) == 0 {
		events = append(events, realRecentEvent{
			Time:     "—",
			Event:    "No canonical operational observations are available yet.",
			Source:   "Dashboard",
			Severity: "INFO",
		})
	}
	return events
}
