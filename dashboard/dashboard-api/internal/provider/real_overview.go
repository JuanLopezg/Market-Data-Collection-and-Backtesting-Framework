package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
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

// One day of the Overview charts. Balance, Expected and CloseLoop are omitted when the
// source does not provide them.
type realEquityPoint struct {
	Label     string   `json:"label"`
	Equity    float64  `json:"equity"`
	Balance   *float64 `json:"balance,omitempty"`
	Expected  *float64 `json:"expected,omitempty"`
	CloseLoop string   `json:"closeLoop,omitempty"`
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

	overview := buildRealOverview(positions, reconciliation, reconciliationErr, execution, executionErr, risk, riskErr, market, marketErr, warnings)
	addPositionValuation(&overview, positions)
	if p.cfg.PaperComparisonFile != "" {
		addPaperComparison(&overview, readPaperComparison(p.cfg.PaperComparisonFile))
	}
	var equityDays []pgstore.EquityDay
	if accounting, err := p.postgres.Accounting(ctx); err == nil && accounting.Available {
		closeLoop, closeLoopErr := p.postgres.CloseLoop(ctx)
		if closeLoopErr != nil {
			overview.SourceWarnings = append(overview.SourceWarnings, "Close loop history unavailable: "+closeLoopErr.Error())
		}
		addRuntimeEquity(&overview, accounting.Equity, closeLoop)
		equityDays = accounting.Equity
	}
	p.addKrakenCollateral(&overview, positions, equityDays)
	orderOverviewStats(&overview)
	return overview, nil
}

// overviewStatOrder is the card order the user chose; cards not listed keep their order after these.
var overviewStatOrder = []string{"Total Equity", "Balance", "Gross Exposure", "Net Exposure", "Collateral", "Cash", "Leverage"}

func orderOverviewStats(overview *realOverviewData) {
	rank := func(label string) int {
		for index, known := range overviewStatOrder {
			if label == known {
				return index
			}
		}
		return len(overviewStatOrder)
	}
	sort.SliceStable(overview.Stats, func(i, j int) bool {
		return rank(overview.Stats[i].Label) < rank(overview.Stats[j].Label)
	})
}

// addRuntimeEquity draws the equity curve from ExecutionState's daily equity (account
// marked at each close), the runtime-owned history. Live vs Expected keeps the PAPER
// comparison, which pairs live and backtest values at the same execution open.
// Each day carries its close loop (CLEAN/BLOCKED) when ExecutionState recorded one; days
// before that history began stay without it rather than being assumed clean.
func addRuntimeEquity(overview *realOverviewData, days []pgstore.EquityDay, closeLoop []pgstore.CloseLoopDay) {
	if len(days) == 0 {
		return
	}
	loopByDay := make(map[uint64]string, len(closeLoop))
	for _, day := range closeLoop {
		loopByDay[day.Date] = day.State
	}
	curve := make([]realEquityPoint, 0, len(days))
	for _, day := range days {
		point := realEquityPoint{Label: formatTradingTimestamp(day.Date), Equity: day.Equity, CloseLoop: loopByDay[day.Date]}
		if day.OpenCost != nil {
			// Balance: equity without the open positions' unrealized PnL.
			balance := day.Cash + *day.OpenCost
			point.Balance = &balance
		}
		curve = append(curve, point)
	}
	overview.EquityCurve, overview.EquityAvailable = curve, true
	if overview.Stats[0].Value == "Not available" {
		overview.Stats[0] = realOverviewMetric{Label: "Total Equity", Value: formatUSD(days[len(days)-1].Equity)}
	}
}

// addPositionValuation shows equity and leverage from positions marked at the latest close.
func addPositionValuation(overview *realOverviewData, positions realPositionsData) {
	if positions.ValuationState != "AVAILABLE" {
		return
	}
	overview.Stats[0] = realOverviewMetric{Label: "Total Equity", Value: positions.TotalEquity}
	if positions.balanceKnown {
		overview.Stats = append(overview.Stats, realOverviewMetric{Label: "Balance", Value: formatUSD(positions.balanceValue),
			Detail: "Total Equity without the open positions' unrealized PnL"})
	}
	overview.Stats[2] = realOverviewMetric{Label: "Leverage", Value: fmt.Sprintf("%.2fx", positions.grossValue/positions.equityValue)}
	// The same figures as the Positions page cards.
	overview.Stats = append(overview.Stats,
		realOverviewMetric{Label: "Gross Exposure", Value: positions.GrossExposure,
			Detail: "Sum of absolute position values (" + positions.GrossExposurePct + " of Total Equity)"},
		realOverviewMetric{Label: "Net Exposure", Value: positions.NetExposure, Detail: "Longs minus shorts"})
}

// addPaperComparison fills equity and Live vs Expected from the daily PAPER comparison:
// account equity marked at each execution open, next to the fast-backtest equity.
func addPaperComparison(overview *realOverviewData, comparison *paperComparison) {
	if comparison == nil || len(comparison.Rows) == 0 {
		return
	}
	curve := make([]realEquityPoint, 0, len(comparison.Rows))
	expected := make([]realEquityPoint, 0, len(comparison.Rows))
	for _, row := range comparison.Rows {
		value := row.ExpectedEquity
		curve = append(curve, realEquityPoint{Label: row.Date, Equity: row.LiveEquity})
		expected = append(expected, realEquityPoint{Label: row.Date, Equity: row.LiveEquity, Expected: &value})
	}
	overview.EquityCurve, overview.LiveExpected = curve, expected
	overview.EquityAvailable, overview.LiveExpectedAvailable = true, true

	if overview.Stats[0].Value != "Not available" {
		return // Already valued at the latest close; the comparison marks are older.
	}
	latest := comparison.Rows[len(comparison.Rows)-1]
	overview.Stats[0] = realOverviewMetric{Label: "Total Equity", Value: formatUSD(latest.LiveEquity)}
	if latest.LiveEquity > 0 {
		gross := 0.0
		for _, position := range comparison.Positions {
			gross += math.Abs(position.LiveUSD)
		}
		overview.Stats[2] = realOverviewMetric{Label: "Leverage", Value: fmt.Sprintf("%.2fx", gross/latest.LiveEquity)}
	}
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
	// Until marked positions exist, leverage is the approved gross target over capital.
	leverage := realOverviewMetric{Label: "Leverage", Value: "Not available"}
	if riskErr == nil && risk.approvedGrossRatioKnown {
		leverage.Value = fmt.Sprintf("%.2fx", risk.approvedGrossRatio)
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

	note := "Overview reads the durable runtime state (trading_runtime_state), the runtime position ledger and daily equity (trading_open_positions, trading_closed_positions, trading_equity_daily written by ExecutionState), PortfolioRisk checkpoints, retained reconciliation evidence and canonical market-data SQLite. Live vs Expected comes from the PAPER comparison."
	if len(warnings) > 0 {
		note += " Some components are degraded; see sourceWarnings."
	}

	return realOverviewData{
		Stats:                 []realOverviewMetric{totalEquity, cash, leverage},
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
			event, severity := fmt.Sprintf("Order %s %s rejected: %s", reject.OrderID, reject.Asset, reject.Reason), "WARN"
			if reject.Kind == "SKIPPED" {
				event, severity = fmt.Sprintf("%s skipped (%s): %s", reject.Asset, reject.Title, reject.Reason), "INFO"
			}
			events = append(events, realRecentEvent{
				Time:     execution.SourceUpdatedAt,
				Event:    event,
				Source:   "Execution",
				Severity: severity,
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
