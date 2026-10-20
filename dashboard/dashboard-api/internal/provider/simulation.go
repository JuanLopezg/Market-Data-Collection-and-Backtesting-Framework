package provider

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"sync"
	"time"
)

const simulationSnapshotVersion = 1
const simulationManualContractVersion = "replay-manual-v1"

type SimulationConfig struct {
	Dir string
	// Optional host collector snapshot (deploy/chain/monitor.py) for the Infrastructure page.
	HostMetricsFile    string
	HostMetricsProject string
}

type simulationReconciliationIssue struct {
	Kind       string `json:"kind"`
	Asset      string `json:"asset"`
	OrderID    string `json:"orderId"`
	LocalValue string `json:"localValue"`
	VenueValue string `json:"venueValue"`
	Message    string `json:"message"`
}

type simulationReconciliation struct {
	State          string                          `json:"state"`
	LocalSequence  uint64                          `json:"localSequence"`
	VenueSequence  uint64                          `json:"venueSequence"`
	LedgerHeadHash string                          `json:"ledgerHeadHash"`
	Issues         []simulationReconciliationIssue `json:"issues"`
}

type simulationPosition struct {
	Asset         string  `json:"asset"`
	Quantity      float64 `json:"quantity"`
	EntryPrice    float64 `json:"entryPrice"`
	MarkPrice     float64 `json:"markPrice"`
	UnrealizedPnL float64 `json:"unrealizedPnl"`
	Leverage      float64 `json:"leverage"`
}

type simulationAccount struct {
	Equity        float64              `json:"equity"`
	MarginUsed    float64              `json:"marginUsed"`
	CashTotal     float64              `json:"cashTotal"`
	CashAvailable float64              `json:"cashAvailable"`
	Positions     []simulationPosition `json:"positions"`
}

type simulationOrder struct {
	OrderID           string  `json:"orderId"`
	StrategyID        uint64  `json:"strategyId"`
	Asset             string  `json:"asset"`
	Side              string  `json:"side"`
	Quantity          float64 `json:"quantity"`
	FilledQuantity    float64 `json:"filledQuantity"`
	RemainingQuantity float64 `json:"remainingQuantity"`
	LimitPrice        float64 `json:"limitPrice"`
	Status            string  `json:"status"`
	NativeOrderID     string  `json:"nativeOrderId"`
	CreatedAt         uint64  `json:"createdAt"`
	ActiveFrom        uint64  `json:"activeFrom"`
}

type simulationFill struct {
	FillID     string  `json:"fillId"`
	OrderID    string  `json:"orderId"`
	StrategyID uint64  `json:"strategyId"`
	Timestamp  uint64  `json:"timestamp"`
	Asset      string  `json:"asset"`
	Side       string  `json:"side"`
	Quantity   float64 `json:"quantity"`
	Price      float64 `json:"price"`
}

type simulationAccounting struct {
	CorrelationID string  `json:"correlationId"`
	Timestamp     uint64  `json:"timestamp"`
	Type          string  `json:"type"`
	Amount        float64 `json:"amount"`
	Asset         string  `json:"asset"`
	NativeFillID  string  `json:"nativeFillId"`
}

type simulationMarketBar struct {
	Asset  string  `json:"asset"`
	Open   float64 `json:"open"`
	High   float64 `json:"high"`
	Low    float64 `json:"low"`
	Close  float64 `json:"close"`
	Volume float64 `json:"volume"`
}

type simulationEquityPoint struct {
	Label     string   `json:"label"`
	Equity    float64  `json:"equity"`
	Balance   *float64 `json:"balance"`   // equity without open-position PnL; absent in older states
	Gross     *float64 `json:"gross"`     // sum of |position value|; absent in older states
	CloseLoop string   `json:"closeLoop"` // reconciliation state that day
}

// Approved target exposure of the latest close decision (USD; negative = short).
type simulationTarget struct {
	Asset string  `json:"asset"`
	USD   float64 `json:"usd"`
}

type simulationLedgerEntry struct {
	Sequence      uint64  `json:"sequence"`
	Kind          string  `json:"kind"`
	EntryID       string  `json:"entryId"`
	EventTime     uint64  `json:"eventTime"`
	Asset         string  `json:"asset"`
	OrderID       string  `json:"orderId"`
	StrategyID    uint64  `json:"strategyId"`
	NativeFillID  string  `json:"nativeFillId"`
	Side          string  `json:"side"`
	PositionDelta float64 `json:"positionDelta"`
	CashDelta     float64 `json:"cashDelta"`
	RealizedPnL   float64 `json:"realizedPnl"`
	GrossNotional float64 `json:"grossNotional"`
	EntryHash     string  `json:"entryHash"`
}

type simulationLedger struct {
	Valid    bool                    `json:"valid"`
	Error    string                  `json:"error"`
	HeadHash string                  `json:"headHash"`
	Entries  []simulationLedgerEntry `json:"entries"`
}

// Operator manual rebalance as published by the replay (see simulation_manual.go).
type simulationManualState struct {
	Ready              bool                     `json:"ready"` // replay running and accepting requests
	Blocker            string                   `json:"blocker"`
	MaxGrossLeverage   float64                  `json:"maxGrossLeverage"` // portfolio-risk limits
	MaxAssetWeight     float64                  `json:"maxAssetWeight"`
	ResyncPending      bool                     `json:"resyncPending"` // manual positions held until the next close
	LastExecuted       string                   `json:"lastExecuted"`  // "<date> open|close"
	LastOrderIDs       string                   `json:"lastOrderIds"`  // "first-last"
	Weights            []simulationManualWeight `json:"weights"`       // last executed target, fraction of equity
	DecisionTimestamp  uint64                   `json:"decisionTimestamp"`
	ExecutionTimestamp uint64                   `json:"executionTimestamp"`
	LastCorrelationID  string                   `json:"lastCorrelationId"`
	LastActor          string                   `json:"lastActor"`
	LastStatus         string                   `json:"lastStatus"` // EXECUTED, REJECTED or SUPERSEDED
	LastDetail         string                   `json:"lastDetail"`
}

type simulationManualWeight struct {
	Asset  string  `json:"asset"`
	Weight float64 `json:"weight"`
}

type simulationSnapshot struct {
	SchemaVersion            int                      `json:"schemaVersion"`
	Generation               uint64                   `json:"generation"`
	Phase                    string                   `json:"phase"`
	HistoricalDate           string                   `json:"historicalDate"`
	BusinessTimestamp        uint64                   `json:"businessTimestamp"`
	DayIndex                 int                      `json:"dayIndex"`
	DayCount                 int                      `json:"dayCount"`
	SourceRowsProcessed      int                      `json:"sourceRowsProcessed"`
	VenueID                  string                   `json:"venueId"`
	Environment              string                   `json:"environment"`
	ReplaySpeed              float64                  `json:"replaySpeed"`
	RouteSafe                bool                     `json:"routeSafe"`
	Reconciliation           simulationReconciliation `json:"reconciliation"`
	Account                  simulationAccount        `json:"account"`
	Orders                   []simulationOrder        `json:"orders"`
	Fills                    []simulationFill         `json:"fills"`
	Accounting               []simulationAccounting   `json:"accounting"`
	Market                   []simulationMarketBar    `json:"market"`
	EquityHistory            []simulationEquityPoint  `json:"equityHistory"`
	Targets                  []simulationTarget       `json:"targets"`
	Pipeline                 *simulationPipeline      `json:"pipeline"` // latest close decision; absent in older states
	Ledger                   simulationLedger         `json:"ledger"`
	RoutableAssets           []string                 `json:"routableAssets"`
	Manual                   simulationManualState    `json:"manual"`
	EconomicFingerprint      string                   `json:"economicFingerprint"`
	StreamFingerprint        string                   `json:"streamFingerprint"`
	Step56RuntimeFingerprint string                   `json:"step56RuntimeFingerprint"`
	Step57ManualFingerprint  string                   `json:"step57ManualFingerprint"`
}

type Simulation struct {
	cfg SimulationConfig

	// Last snapshot that decoded and validated, kept to bridge torn reads.
	mu       sync.Mutex
	lastGood *simulationSnapshot
	lastAt   time.Time
}

// The replay rewrites state.json many times per second. On bind mounts (Docker Desktop
// over Windows) the tmp+rename is not atomic for readers, so a read can see a partial
// file. Retry briefly, then serve the last good snapshot if it is recent; never older.
const (
	simulationReadAttempts = 3
	simulationRetryDelay   = 30 * time.Millisecond
	simulationStaleLimit   = 10 * time.Second
)

func NewSimulation(cfg SimulationConfig) *Simulation {
	return &Simulation{cfg: cfg}
}

func (p *Simulation) snapshotPath() string {
	return filepath.Join(p.cfg.Dir, "state.json")
}

func (p *Simulation) load() (simulationSnapshot, error) {
	var err error
	for attempt := 0; attempt < simulationReadAttempts; attempt++ {
		var s simulationSnapshot
		if s, err = p.loadOnce(); err == nil {
			p.mu.Lock()
			p.lastGood, p.lastAt = &s, time.Now()
			p.mu.Unlock()
			return s, nil
		}
		time.Sleep(simulationRetryDelay)
	}
	p.mu.Lock()
	defer p.mu.Unlock()
	if p.lastGood != nil && time.Since(p.lastAt) < simulationStaleLimit {
		return *p.lastGood, nil
	}
	return simulationSnapshot{}, err
}

func (p *Simulation) loadOnce() (simulationSnapshot, error) {
	raw, err := os.ReadFile(p.snapshotPath())
	if err != nil {
		return simulationSnapshot{}, err
	}
	var s simulationSnapshot
	if err := json.Unmarshal(raw, &s); err != nil {
		return simulationSnapshot{}, fmt.Errorf("decode simulation state: %w", err)
	}
	if s.SchemaVersion != simulationSnapshotVersion || s.Generation == 0 || s.VenueID != "MOCK" || s.Environment != "MOCK" {
		return simulationSnapshot{}, errors.New("simulation state identity/version is invalid")
	}
	if !finiteSimulation(s.Account.Equity) || !finiteSimulation(s.Account.MarginUsed) || !finiteSimulation(s.Account.CashTotal) || !finiteSimulation(s.Account.CashAvailable) {
		return simulationSnapshot{}, errors.New("simulation account contains non-finite values")
	}
	return s, nil
}

func (p *Simulation) Health(_ context.Context) Health {
	s, err := p.load()
	if err != nil {
		return Health{Name: "simulation-provider-step58", Mode: "mock", Ready: false, Detail: "waiting for Step58 simulation state: " + err.Error(), ResourceCount: len(AllResources)}
	}
	return Health{Name: "simulation-provider-step58", Mode: "mock", Ready: s.Phase != "ERROR", Detail: fmt.Sprintf("MOCK replay generation=%d phase=%s historical=%s", s.Generation, s.Phase, s.HistoricalDate), ResourceCount: len(AllResources)}
}

func (p *Simulation) Read(_ context.Context, resource Resource) (json.RawMessage, error) {
	s, err := p.load()
	if err != nil {
		return nil, fmt.Errorf("%w: simulation snapshot: %v", ErrResourceUnavailable, err)
	}
	var value any
	switch resource {
	case ResourceShellStatus:
		value = p.shell(s)
	case ResourceOverview:
		value = p.overview(s)
	case ResourcePositions:
		value = p.positions(s)
	case ResourceReconciliation:
		value = p.reconciliation(s)
	case ResourcePipeline:
		value = p.pipeline(s)
	case ResourceExecution:
		value = p.execution(s)
	case ResourceRisk:
		value = p.risk(s)
	case ResourceMarketData:
		value = p.marketData(s)
	case ResourceInfrastructure:
		value = p.infrastructure(s)
	case ResourceAlertsAudit:
		value = p.alertsAudit(s)
	case ResourceLiveVsExpected:
		value = p.liveVsExpected(s)
	case ResourceManualControl:
		data, e := p.manualBase(s, "authenticated OPERATOR session")
		if e != nil {
			return nil, e
		}
		value = data
	default:
		return nil, fmt.Errorf("%w: %s", ErrIntegrationNotImplemented, resource)
	}
	raw, err := json.Marshal(value)
	if err != nil {
		return nil, err
	}
	return raw, nil
}

func (p *Simulation) SubscribeDashboardEvents(ctx context.Context, notify func(string)) error {
	ticker := time.NewTicker(250 * time.Millisecond)
	defer ticker.Stop()
	var generation uint64
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			s, err := p.load()
			if err != nil {
				continue
			}
			if generation == 0 {
				generation = s.Generation
				continue
			}
			if s.Generation != generation {
				generation = s.Generation
				notify("simulation.step58.generation")
			}
		}
	}
}

func (p *Simulation) shell(s simulationSnapshot) map[string]any {
	recon := normalizedReconState(s.Reconciliation.State)
	readiness := "READY"
	if recon == "BLOCKED" {
		readiness = "PAUSED"
	}
	if recon == "PENDING" {
		readiness = "DEGRADED"
	}
	alerts := 0
	critical := 0
	warnings := 0
	if recon == "BLOCKED" {
		alerts++
		critical++
	}
	if recon == "PENDING" {
		alerts++
		warnings++
	}
	return map[string]any{
		"mode": "REPLAY", "exchange": "MOCK", "exchangeConnected": s.Phase != "ERROR",
		"exchangeState": s.Phase, "exchangeDetail": "Step58 canonical MOCK simulation provider",
		"dataHealthy": s.Phase != "ERROR", "dataState": "VALID", "dataDetail": fmt.Sprintf("historical %s · generation %d", s.HistoricalDate, s.Generation),
		"tradingEnabled": s.RouteSafe, "tradingState": map[bool]string{true: "REPLAY ROUTE SAFE", false: "REPLAY PAUSED"}[s.RouteSafe],
		"tradingDetail":  "New exposure is gated by Step54/55 reconciliation; this is MOCK only.",
		"reconciliation": recon, "readiness": readiness, "alertCount": alerts,
		"criticalAlertCount": critical, "warningAlertCount": warnings,
		"utcLabel": fmt.Sprintf("BUSINESS %d · %s", s.BusinessTimestamp, s.HistoricalDate),
		"blockers": reconciliationMessages(s), "warnings": []string{}, "sourceMode": "MOCK",
	}
}

func (p *Simulation) overview(s simulationSnapshot) map[string]any {
	positions := dashboardPositions(s)
	unrealized := totalUnrealized(s)
	stats := []map[string]any{
		{"label": "Total Equity", "value": formatUSD2(s.Account.Equity)},
		{"label": "Cash", "value": formatUSD2(s.Account.CashTotal)},
		{"label": "Unrealized PnL", "value": formatSignedUSD(unrealized)},
		{"label": "Leverage", "value": fmt.Sprintf("%.2fx", simulationLeverage(s))},
	}
	curve := make([]map[string]any, 0, len(s.EquityHistory))
	for _, point := range s.EquityHistory {
		row := map[string]any{"label": point.Label, "equity": point.Equity, "closeLoop": point.CloseLoop}
		if point.Balance != nil {
			row["balance"] = *point.Balance
		}
		curve = append(curve, row)
	}
	liveExpected := p.liveExpectedRows(s)
	events := []map[string]any{{"time": fmt.Sprintf("%d", s.BusinessTimestamp), "event": fmt.Sprintf("%s · day %d/%d", s.Phase, s.DayIndex, s.DayCount), "source": "Step58 replay", "severity": "INFO"}}
	if s.Manual.LastCorrelationID != "" {
		events = append(events, map[string]any{"time": fmt.Sprintf("%d", s.BusinessTimestamp), "event": s.Manual.LastStatus + " · " + s.Manual.LastDetail, "source": "Manual Control", "severity": "INFO"})
	}
	return map[string]any{
		"stats": stats, "equityCurve": curve, "liveExpected": liveExpected, "positions": positions, "recentEvents": events,
		"checks":     []map[string]any{{"label": "Reconciliation", "state": alignmentFromRecon(s.Reconciliation.State), "detail": fmt.Sprintf("sequence %d/%d", s.Reconciliation.LocalSequence, s.Reconciliation.VenueSequence)}},
		"sourceMode": "MOCK", "sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp),
		"sourceNote":      "Step58 reads canonical MOCK runtime state; no exchange/PG/NATS values are fabricated.",
		"readiness":       map[bool]string{true: "READY", false: "PAUSED"}[s.RouteSafe],
		"readinessDetail": s.Phase, "reconciliationStatus": normalizedReconState(s.Reconciliation.State),
		"equityAvailable": true, "liveExpectedAvailable": len(liveExpected) > 0, "marketDataStatus": "ALIGNED",
		"canonicalCycle": fmt.Sprintf("step58-%s-%d", s.HistoricalDate, s.Generation), "sourceWarnings": []string{},
	}
}

// Gross position value over equity, e.g. 0.30x when 30% of equity is invested.
func simulationLeverage(s simulationSnapshot) float64 {
	gross := 0.0
	for _, position := range s.Account.Positions {
		gross += math.Abs(position.Quantity * position.MarkPrice)
	}
	if s.Account.Equity <= 0 {
		return 0
	}
	return gross / s.Account.Equity
}

// expectedEquity reads expected_equity.csv (date,balance,equity), written by replay.py
// from the fast backtest of the same window. Missing or unreadable file: no expected line.
func (p *Simulation) expectedEquity() map[string]float64 {
	raw, err := os.ReadFile(filepath.Join(p.cfg.Dir, "expected_equity.csv"))
	if err != nil {
		return nil
	}
	expected := map[string]float64{}
	for _, line := range strings.Split(strings.TrimSpace(string(raw)), "\n")[1:] {
		fields := strings.Split(strings.TrimSpace(line), ",")
		if len(fields) != 3 {
			continue
		}
		if equity, err := strconv.ParseFloat(fields[2], 64); err == nil && finiteSimulation(equity) {
			expected[fields[0]] = equity
		}
	}
	return expected
}

// Replayed ("live") equity next to the fast-backtest equity for the same days.
func (p *Simulation) liveExpectedRows(s simulationSnapshot) []map[string]any {
	expected := p.expectedEquity()
	rows := make([]map[string]any, 0, len(s.EquityHistory))
	if len(expected) == 0 {
		return rows
	}
	for _, point := range s.EquityHistory {
		row := map[string]any{"label": point.Label, "equity": point.Equity}
		if value, ok := expected[point.Label]; ok {
			row["expected"] = value
		}
		rows = append(rows, row)
	}
	return rows
}

func (p *Simulation) positions(s simulationSnapshot) map[string]any {
	gross := 0.0
	net := 0.0
	for _, pos := range s.Account.Positions {
		notional := pos.Quantity * pos.MarkPrice
		gross += math.Abs(notional)
		net += notional
	}
	grossPct := 0.0
	if s.Account.Equity != 0 {
		grossPct = gross / s.Account.Equity * 100
	}
	closed, _ := buildPositionHistory(simulationTradeEvents(s))
	return map[string]any{
		"closedPositions": closedPositionRows(closed), "closedAvailable": true, "fundingAvailable": true,
		"totalEquity": formatUSD2(s.Account.Equity), "activePositions": len(s.Account.Positions),
		"grossExposure": formatUSD2(gross), "grossExposurePct": fmt.Sprintf("%.2f%%", grossPct),
		"netExposure": formatSignedUSD(net), "unrealizedPnl": formatSignedUSD(totalUnrealized(s)),
		"positions": dashboardPositions(s), "accountCash": formatUSD2(s.Account.CashTotal), "sourceMode": "MOCK",
		"sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp), "valuationState": "AVAILABLE",
		"sourceNote": "Step52 canonical linear-perpetual account snapshot.",
	}
}

// Half a unit of the MOCK 8-decimal quantity grid: ledger deltas are summed as doubles.
const simulationReconQuantityTolerance = 0.5e-8

// Reconciliation compares our books with the exchange, per asset: "ours" is the local
// ledger the reconciler rebuilds from the event stream (sum of fill position deltas),
// "exchange" is the MOCK venue account. The overall verdict and issues are the C++
// reconciler's; a row only adds its own DIFFERENT when the two quantities disagree.
func (p *Simulation) reconciliation(s simulationSnapshot) map[string]any {
	ours := map[string]float64{}
	for _, entry := range s.Ledger.Entries {
		if entry.Kind == "FILL" {
			ours[entry.Asset] += entry.PositionDelta
		}
	}
	exchange := map[string]float64{}
	for _, pos := range s.Account.Positions {
		exchange[pos.Asset] = pos.Quantity
	}
	pending := map[string]float64{}
	open := make([]map[string]any, 0)
	for _, o := range s.Orders {
		if !isOpenStatus(o.Status) {
			continue
		}
		signed := o.RemainingQuantity
		if o.Side == "SELL" {
			signed = -signed
		}
		pending[o.Asset] += signed
		open = append(open, map[string]any{"orderId": o.OrderID, "asset": o.Asset, "side": o.Side, "quantity": o.Quantity, "remaining": o.RemainingQuantity, "age": tradeTimeLabel(o.ActiveFrom), "state": executionOrderState(o.Status), "exchangeId": o.NativeOrderID})
	}

	assets := map[string]struct{}{}
	for _, values := range []map[string]float64{ours, exchange, pending} {
		for asset, value := range values {
			if math.Abs(value) > simulationReconQuantityTolerance {
				assets[asset] = struct{}{}
			}
		}
	}
	ordered := make([]string, 0, len(assets))
	for asset := range assets {
		ordered = append(ordered, asset)
	}
	sort.Strings(ordered)

	rows := make([]map[string]any, 0, len(ordered))
	for _, asset := range ordered {
		local, venue := ours[asset], exchange[asset]
		delta := venue - local
		status, explanation := alignmentFromRecon(s.Reconciliation.State), "Our ledger and the exchange hold the same quantity."
		if math.Abs(delta) > simulationReconQuantityTolerance {
			status, explanation = "BLOCKED", "Our ledger and the exchange hold different quantities."
		} else {
			delta = 0
		}
		rows = append(rows, map[string]any{
			"asset": asset, "localQty": local, "localQtyLabel": formatQuantity(local),
			"exchangeQty": venue, "exchangeQtyLabel": formatQuantity(venue), "exchangeAvailable": true,
			"deltaQty": delta, "deltaQtyLabel": signedQuantity(delta),
			"pendingQty": pending[asset], "pendingQtyLabel": signedQuantity(pending[asset]),
			"status": status, "explanation": explanation,
		})
	}
	issues := make([]map[string]any, 0, len(s.Reconciliation.Issues))
	for _, i := range s.Reconciliation.Issues {
		issues = append(issues, map[string]any{"kind": i.Kind, "asset": i.Asset, "orderId": i.OrderID, "localValue": 0, "exchangeValue": 0, "message": i.Message + optionalCompare(i.LocalValue, i.VenueValue)})
	}
	return map[string]any{
		"status": normalizedReconState(s.Reconciliation.State), "lastChecked": tradeTimeLabel(s.BusinessTimestamp), "tolerance": "exact on the 8-decimal grid",
		"rows": rows, "openOrders": open, "sourceMode": "MOCK", "sourceUpdatedAt": tradeTimeLabel(s.BusinessTimestamp),
		"comparisonAvailable": true, "evidenceFresh": s.Reconciliation.LocalSequence == s.Reconciliation.VenueSequence,
		"exchangeEvidenceSequence": s.Reconciliation.VenueSequence, "issues": issues,
	}
}

func (p *Simulation) execution(s simulationSnapshot) map[string]any {
	orders := make([]map[string]any, 0, len(s.Orders))
	filled := 0
	partial := 0
	open := 0
	pendingCancel := 0
	rejects := 0
	feesByFill := accountingFees(s)
	limitByOrder := map[string]simulationOrder{}
	for _, o := range s.Orders {
		limitByOrder[o.OrderID] = o
	}
	// Per order: quantity-weighted average fill price and fees.
	fillQty, fillValue, feeByOrder := map[string]float64{}, map[string]float64{}, map[string]float64{}
	for _, f := range s.Fills {
		fillQty[f.OrderID] += f.Quantity
		fillValue[f.OrderID] += f.Quantity * f.Price
		feeByOrder[f.OrderID] += feesByFill[f.FillID]
	}
	slippage := simulationSlippage(s.Fills, limitByOrder)
	for _, o := range s.Orders {
		state := executionOrderState(o.Status)
		switch state {
		case "FILLED":
			filled++
		case "PARTIAL":
			partial++
			open++
		case "PENDING_CANCEL":
			pendingCancel++
			open++
		case "NEW":
			open++
		case "REJECTED":
			rejects++
		}
		avg, slippageBps, slippageLabel := "—", 0.0, "—"
		if fillQty[o.OrderID] > 0 {
			price := fillValue[o.OrderID] / fillQty[o.OrderID]
			avg = formatPrice(price)
			if bps, ok := slippageBpsFor(o.Side, o.LimitPrice, price); ok {
				slippageBps, slippageLabel = bps, formatBps(bps)
			}
		}
		orders = append(orders, map[string]any{
			"orderId": o.OrderID, "strategyId": o.StrategyID, "exchangeOrderId": o.NativeOrderID, "cycleId": fmt.Sprintf("step58-%s", o.OrderID), "correlationId": fmt.Sprintf("mock-order-%s", o.OrderID),
			"asset": o.Asset, "side": o.Side, "quantity": o.Quantity, "filledQty": o.FilledQuantity, "remainingQty": o.RemainingQuantity, "state": state, "age": "business-event",
			"expectedPriceLabel": formatPrice(o.LimitPrice), "avgFillPriceLabel": avg, "feesLabel": formatUSD2(feeByOrder[o.OrderID]), "slippageBps": slippageBps, "slippageBpsLabel": slippageLabel, "submitLatencyMs": 0, "fillLatencyMs": nil,
			"submittedAt": fmt.Sprintf("%d", o.ActiveFrom), "lastUpdateAt": fmt.Sprintf("%d", s.BusinessTimestamp), "lifecycle": []any{},
		})
	}
	// Newest first.
	for i, j := 0, len(orders)-1; i < j; i, j = i+1, j-1 {
		orders[i], orders[j] = orders[j], orders[i]
	}
	fills := make([]map[string]any, 0, len(s.Fills))
	totalFees := 0.0
	for _, f := range s.Fills {
		fee := feesByFill[f.FillID]
		totalFees += fee
		fills = append(fills, map[string]any{"fillId": f.FillID, "orderId": f.OrderID, "asset": f.Asset, "quantity": f.Quantity, "priceLabel": formatPrice(f.Price), "cumulativeLabel": formatQty(f.Quantity), "feesLabel": formatUSD2(fee), "timestamp": fmt.Sprintf("%d", f.Timestamp)})
	}
	return map[string]any{
		"lastUpdated": fmt.Sprintf("%d", s.BusinessTimestamp), "openOrders": open, "partialOrders": partial, "pendingCancels": pendingCancel, "filledOrders": filled, "fillCount": len(s.Fills), "rejectCount": rejects,
		"avgSubmitLatencyMs": 0, "avgFillLatencyMs": 0, "avgSlippageBpsLabel": slippage.averageLabel, "submitLatencyP95Ms": 0, "fillLatencyP95Ms": 0, "bestSlippageBpsLabel": slippage.bestLabel, "worstSlippageBpsLabel": slippage.worstLabel,
		"totalFeesLabel": formatUSD2(totalFees), "rejectRateLabel": percent(rejects, len(s.Orders)), "orders": orders, "partialFills": fills, "rejects": []any{}, "replacements": []any{},
		"slippageHistory": slippage.daily,
		"sourceMode":      "MOCK", "sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp), "sourceNote": "Replay has no wall-clock latency. Slippage = fill price against the order's expected price (open of the execution day).", "latencyAvailable": false, "slippageAvailable": slippage.available, "orderWindowTruncated": false, "fillWindowTruncated": false,
	}
}

// Execution slippage in basis points against the expected price; positive = adverse
// (paid more on a buy, received less on a sell).
func slippageBpsFor(side string, expected, fill float64) (float64, bool) {
	if !finitePositive(expected) || !finitePositive(fill) {
		return 0, false
	}
	bps := (fill - expected) / expected * 10000
	if side == "SELL" {
		bps = -bps
	}
	return bps, isFiniteLedger(bps)
}

func formatBps(value float64) string {
	if math.Abs(value) < 0.005 {
		return "0.00 bps"
	}
	return fmt.Sprintf("%+.2f bps", value)
}

type simulationSlippageSummary struct {
	available                           bool
	averageLabel, bestLabel, worstLabel string
	daily                               []map[string]any // {label: YYYY-MM-DD, bps, fills}
}

// Daily sums, a notional-weighted overall average, and fill best/worst.
func simulationSlippage(fills []simulationFill, orders map[string]simulationOrder) simulationSlippageSummary {
	type day struct {
		bps   float64
		fills int
	}
	days := map[string]*day{}
	totalWeighted, totalNotional := 0.0, 0.0
	best, worst := math.Inf(1), math.Inf(-1)
	for _, fill := range fills {
		order, ok := orders[fill.OrderID]
		if !ok {
			continue
		}
		bps, ok := slippageBpsFor(order.Side, order.LimitPrice, fill.Price)
		if !ok {
			continue
		}
		notional := fill.Quantity * fill.Price
		// Replay business time is YYYYMMDD plus a phase digit.
		label := formatTradingTimestamp(fill.Timestamp / 10)
		if days[label] == nil {
			days[label] = &day{}
		}
		days[label].bps += bps
		days[label].fills++
		totalWeighted += bps * notional
		totalNotional += notional
		best, worst = math.Min(best, bps), math.Max(worst, bps)
	}
	summary := simulationSlippageSummary{averageLabel: "—", bestLabel: "—", worstLabel: "—", daily: []map[string]any{}}
	if totalNotional <= 0 {
		return summary
	}
	summary.available = true
	summary.averageLabel, summary.bestLabel, summary.worstLabel = formatBps(totalWeighted/totalNotional), formatBps(best), formatBps(worst)
	labels := make([]string, 0, len(days))
	for label := range days {
		labels = append(labels, label)
	}
	sort.Strings(labels)
	for _, label := range labels {
		d := days[label]
		summary.daily = append(summary.daily, map[string]any{"label": label, "bps": d.bps, "fills": d.fills})
	}
	return summary
}

func (p *Simulation) marketData(s simulationSnapshot) map[string]any {
	freshness := make([]map[string]any, 0, len(s.Market))
	for _, b := range s.Market {
		freshness = append(freshness, map[string]any{"asset": b.Asset, "lastCandle": s.HistoricalDate, "ageLabel": "0d", "staleThresholdLabel": "event-time sequence", "source": "frozen OHLCV CSV", "state": "HEALTHY"})
	}
	return map[string]any{
		"source": "Frozen historical OHLCV / TimeHandler", "latestCompletedCandle": s.HistoricalDate, "healthyAssets": len(s.Market), "staleAssets": 0, "totalAssets": len(s.Market), "universeSize": 20, "activeSignals": len(s.Account.Positions), "availableSlots": maxInt(0, 10-len(s.Account.Positions)),
		"freshness": freshness, "integrity": map[string]any{"missingCandles": 0, "duplicateTimestamps": 0, "gaps": 0, "invalidRows": 0}, "universe": []any{}, "candidateRejections": []any{},
		"strategySnapshot": map[string]any{"ranking": "Top-20 SMA Volume(25)", "entryRule": "RSI(7) > 80", "exitRule": "RSI(7) < 70", "maxPositions": "10 persistent LEVEL signals", "semantics": "PureRSI frozen semantics"},
		"sourceMode":       "MOCK", "sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp), "sourceNote": "Current closed bars are canonical. RSI/ranking internals are not re-derived in dashboard.", "signalDataAvailable": false, "signalCycleAligned": true, "canonicalTopN": 20, "historyDays": 0,
	}
}

func (p *Simulation) infrastructure(s simulationSnapshot) map[string]any {
	state := "HEALTHY"
	if s.Phase == "ERROR" {
		state = "CRITICAL"
	}
	result := map[string]any{
		"readiness": map[bool]string{true: "READY", false: "PAUSED"}[s.RouteSafe], "readinessReason": "Step58 local simulation read model", "lastUpdated": fmt.Sprintf("%d", s.BusinessTimestamp),
		"vps":        map[string]any{"cpuPct": 0, "ramPct": 0, "diskPct": 0, "load1m": 0, "networkRxLabel": "not sampled", "networkTxLabel": "not sampled", "uptimeLabel": "local simulation", "clockOffsetLabel": "not used for economics", "clockSynced": true, "state": "UNKNOWN"},
		"containers": []any{}, "services": []map[string]any{{"service": "Replay runner", "ready": s.Phase != "ERROR", "mode": "REPLAY", "lastEvent": fmt.Sprintf("%d", s.BusinessTimestamp), "lagLabel": "generation-driven", "health": state}},
		"postgres":     map[string]any{"connected": false, "latencyMs": 0, "activeConnections": 0, "maxConnections": 0, "storageUsedLabel": "not used", "storagePct": 0, "persistenceState": "Step53 file recovery", "state": "UNKNOWN"},
		"nats":         map[string]any{"connected": false, "streams": 0, "consumers": 0, "pendingMessages": 0, "maxConsumerLag": 0, "redeliveries": 0, "ackHealthLabel": "not used in local Step58 simulation bridge", "state": "UNKNOWN"},
		"exchange":     map[string]any{"venue": "MOCK", "connected": s.Phase != "ERROR", "reconnectState": "DETERMINISTIC", "lastApiActivity": fmt.Sprintf("%d", s.BusinessTimestamp), "lastWsActivity": fmt.Sprintf("%d", s.BusinessTimestamp), "apiLatencyMs": 0, "state": state},
		"outbox":       map[string]any{"pendingMessages": 0, "oldestAgeLabel": "0", "lastPublished": "file generation", "state": "HEALTHY"},
		"dependencies": []map[string]any{{"component": "Step53 recovery", "state": state, "reason": "canonical MOCK durable state", "lastCheck": fmt.Sprintf("%d", s.BusinessTimestamp)}, {"component": "Step54 reconciliation", "state": map[string]string{"CLEAN": "HEALTHY", "PENDING": "WARN", "BLOCKED": "CRITICAL"}[s.Reconciliation.State], "reason": s.Reconciliation.State, "lastCheck": fmt.Sprintf("%d", s.BusinessTimestamp)}},
		"sourceMode":   "MOCK", "sourceNote": "Host/container telemetry is intentionally not fabricated by the simulation read-model provider.",
	}
	p.applyHostTelemetry(result)
	return result
}

// Host CPU/RAM/disk and dashboard-container status from the host collector, when configured
// and fresh; otherwise the host stays "not sampled".
func (p *Simulation) applyHostTelemetry(result map[string]any) {
	if p.cfg.HostMetricsFile == "" {
		return
	}
	snapshot, err := readHostTelemetry(p.cfg.HostMetricsFile, p.cfg.HostMetricsProject, time.Now())
	if err != nil {
		return
	}
	result["vps"], result["containers"], result["telemetryObservedAt"] = *snapshot.VPS, snapshot.Containers, snapshot.ObservedAt
}

func (p *Simulation) liveVsExpected(s simulationSnapshot) map[string]any {
	// Same shape as the PAPER comparison: replayed equity against the fast backtest.
	expected := p.expectedEquity()
	rows := []paperComparisonRow{}
	for _, point := range s.EquityHistory {
		if value, ok := expected[point.Label]; ok {
			rows = append(rows, paperComparisonRow{Date: point.Label, LiveEquity: point.Equity, ExpectedEquity: value})
		}
	}
	comparison := &paperComparison{ContractVersion: "paper-comparison-v1", Status: "REPLAY", Profile: "Canonical replay vs fast backtest",
		CheckedAt: s.HistoricalDate, Rows: rows, Positions: []paperComparisonPosition{}}
	if len(expected) == 0 {
		comparison.Status = "UNAVAILABLE"
	}
	return map[string]any{"paperComparison": comparison, "contractVersion": "step58-simulation-v1", "status": "INSUFFICIENT_DATA", "validated": true, "projectionReady": false, "observationMode": "MOCK_REPLAY", "baselineLabel": "Not fabricated", "baselineWindow": "Step59 will validate deterministic baselines/speed invariance", "baselineFingerprint": "", "baselineObservationCount": 0, "baselineExcludesLatest": true, "latestObservation": s.HistoricalDate, "metricCount": 0, "normalCount": 0, "elevatedCount": 0, "abnormalCount": 0, "criticalCount": 0, "overallClassification": "INSUFFICIENT_DATA", "anomalyCount": 0, "anomalies": []any{}, "coverage": []map[string]any{{"id": "execution", "label": "Execution/accounting baseline", "state": "DEFERRED", "source": "Step59", "detail": "Step58 exposes live canonical values but does not fabricate an accepted statistical baseline."}}, "metrics": []any{}, "sourceMode": "MOCK", "sourceNote": "No baseline classification is invented.", "readOnly": true, "privateAuth": "DISABLED", "orderRouting": "MOCK_ONLY", "checkedAt": fmt.Sprintf("%d", s.BusinessTimestamp)}
}

func (p *Simulation) LedgerStatus(_ context.Context) LedgerStatus {
	s, err := p.load()
	if err != nil {
		return LedgerStatus{Status: "BLOCKED", ReadOnly: true, PrivateAuth: "DISABLED", OrderRouting: "MOCK_ONLY", CheckedAt: time.Now().UTC().Format(time.RFC3339), Error: err.Error(), Entries: []LedgerEntry{}, Note: "Step58 simulation snapshot unavailable."}
	}
	result := LedgerStatus{Status: "VALIDATED", Validated: s.Ledger.Valid, FoundationReady: s.Ledger.Valid, SourceTable: "Step54 canonical MOCK user stream", SourceContract: "Step54 FILL + TRADING_FEE/REBATE/FUNDING hash chain; dashboard rows fold accounting into their related fill where possible", AppendOnlySource: true, DeterministicProjection: true, RecentWindowFingerprint: s.Ledger.HeadHash, DurableRealizedPnL: true, DurableUnrealizedPnL: false, HistoricalEquityAvailable: false, ReadOnly: true, PrivateAuth: "DISABLED", OrderRouting: "MOCK_ONLY", CheckedAt: fmt.Sprintf("%d", s.BusinessTimestamp), Entries: []LedgerEntry{}, Note: "Canonical Step54 ledger parity. Perpetual principal notional does not move settled cash; cashDelta reflects realized PnL plus linked accounting events, not spot purchase/sale principal."}
	if !s.Ledger.Valid {
		result.Status = "BLOCKED"
		result.FoundationReady = false
		result.Error = s.Ledger.Error
		return result
	}
	feeByFill := map[string]float64{}
	cashAccountingByFill := map[string]float64{}
	for _, e := range s.Ledger.Entries {
		if e.Kind == "TRADING_FEE" || e.Kind == "REBATE" || e.Kind == "FUNDING_PAYMENT" {
			cashAccountingByFill[e.NativeFillID] += e.CashDelta
			if e.Kind == "TRADING_FEE" {
				feeByFill[e.NativeFillID] += -e.CashDelta
			}
		}
	}
	fillsByID := make(map[string]simulationFill, len(s.Fills))
	for _, f := range s.Fills {
		fillsByID[f.FillID] = f
	}
	for _, e := range s.Ledger.Entries {
		if e.Kind != "FILL" {
			continue
		}
		commission := feeByFill[e.NativeFillID]
		row := LedgerEntry{EntryID: e.EntryID, FillID: e.NativeFillID, OrderID: e.OrderID, StrategyID: e.StrategyID, Timestamp: fmt.Sprintf("%d", e.EventTime), Asset: e.Asset, Side: e.Side, Quantity: math.Abs(e.PositionDelta), Price: 0, GrossNotional: e.GrossNotional, Commission: commission, PositionDelta: e.PositionDelta, CashDelta: e.CashDelta + cashAccountingByFill[e.NativeFillID], EntryHash: e.EntryHash}
		if f, ok := fillsByID[e.NativeFillID]; ok {
			row.Quantity = f.Quantity
			row.Price = f.Price
		}
		result.Entries = append(result.Entries, row)
		result.TotalFillRows++
		result.DistinctFillIDs++
		result.TotalFees += commission
		if e.Side == "BUY" {
			result.GrossBuyNotional += e.GrossNotional
		} else if e.Side == "SELL" {
			result.GrossSellNotional += e.GrossNotional
		}
		result.NetCashDeltaFromFills += row.CashDelta
	}
	for i, j := 0, len(result.Entries)-1; i < j; i, j = i+1, j-1 {
		result.Entries[i], result.Entries[j] = result.Entries[j], result.Entries[i]
	}
	result.RecentWindowCount = len(result.Entries)
	result.TotalFeesLabel = formatUSD2(result.TotalFees)
	return result
}

func finiteSimulation(v float64) bool { return !math.IsNaN(v) && !math.IsInf(v, 0) }
func formatUSD2(v float64) string     { return fmt.Sprintf("$%.2f", v) }
func formatSignedUSD(v float64) string {
	if v >= 0 {
		return fmt.Sprintf("+$%.2f", v)
	}
	return fmt.Sprintf("-$%.2f", math.Abs(v))
}
func formatQty(v float64) string { return strconv.FormatFloat(v, 'f', 8, 64) }
func formatPrice(v float64) string {
	if v <= 0 {
		return "—"
	}
	return fmt.Sprintf("$%.8f", v)
}
func safePct(v, total float64) float64 {
	if total == 0 {
		return 0
	}
	return v / total * 100
}
func percent(part, total int) string {
	if total == 0 {
		return "0.00%"
	}
	return fmt.Sprintf("%.2f%%", float64(part)/float64(total)*100)
}
func totalUnrealized(s simulationSnapshot) float64 {
	v := 0.0
	for _, p := range s.Account.Positions {
		v += p.UnrealizedPnL
	}
	return v
}
func normalizedReconState(v string) string {
	switch v {
	case "CLEAN":
		return "CLEAN"
	case "BLOCKED":
		return "BLOCKED"
	default:
		return "PENDING"
	}
}
func alignmentFromRecon(v string) string {
	switch v {
	case "CLEAN":
		return "ALIGNED"
	case "BLOCKED":
		return "BLOCKED"
	default:
		return "PENDING"
	}
}
func isOpenStatus(v string) bool {
	return v == "ACCEPTED" || v == "RESTING" || v == "PARTIALLY_FILLED" || v == "CANCEL_PENDING" || v == "PENDING_SUBMIT"
}
func executionOrderState(v string) string {
	switch v {
	case "FILLED":
		return "FILLED"
	case "PARTIALLY_FILLED":
		return "PARTIAL"
	case "CANCEL_PENDING":
		return "PENDING_CANCEL"
	case "CANCELED":
		return "CANCELED"
	case "REJECTED", "VENUE_TERMINATED", "UNKNOWN_REQUIRES_RECONCILIATION":
		return "REJECTED"
	default:
		return "NEW"
	}
}
func countOpenOrders(s simulationSnapshot) int {
	n := 0
	for _, o := range s.Orders {
		if isOpenStatus(o.Status) {
			n++
		}
	}
	return n
}
func optionalCompare(local, venue string) string {
	if local == "" && venue == "" {
		return ""
	}
	return fmt.Sprintf(" [local=%s venue=%s]", local, venue)
}
func reconciliationMessages(s simulationSnapshot) []string {
	out := []string{}
	for _, i := range s.Reconciliation.Issues {
		out = append(out, i.Message)
	}
	if len(out) == 0 && s.Reconciliation.State != "CLEAN" {
		out = append(out, "Reconciliation evidence is not CLEAN")
	}
	return out
}
func accountingFees(s simulationSnapshot) map[string]float64 {
	out := map[string]float64{}
	for _, e := range s.Accounting {
		if e.Type == "TRADING_FEE" && e.NativeFillID != "" {
			out[e.NativeFillID] += -e.Amount
		}
	}
	return out
}
func manualAuditResult(status string) string {
	if status == "EXECUTED" {
		return "SUCCESS"
	}
	return "REJECTED"
}
func currentWeights(s simulationSnapshot) map[string]float64 {
	out := map[string]float64{}
	used := 0.0
	if s.Account.Equity != 0 {
		for _, p := range s.Account.Positions {
			w := p.Quantity * p.MarkPrice / s.Account.Equity * 100
			out[p.Asset] = w
			used += w
		}
	}
	out["CASH"] = 100 - used
	return out
}
func dashboardPositions(s simulationSnapshot) []map[string]any {
	// Target weight = latest approved target exposure / current equity; missing asset = 0.
	// States written before targets existed (nil list) report the target as unavailable.
	targets := map[string]float64{}
	for _, target := range s.Targets {
		targets[target.Asset] = target.USD
	}
	targetsAvailable := s.Targets != nil
	// Fees, funding and partial-close PnL so far come from the position history.
	_, lots := buildPositionHistory(simulationTradeEvents(s))
	out := make([]map[string]any, 0, len(s.Account.Positions))
	for _, p := range s.Account.Positions {
		side := "Long"
		q := p.Quantity
		if q < 0 {
			side = "Short"
		}
		pnlPct := 0.0
		if p.EntryPrice > 0 {
			if q >= 0 {
				pnlPct = (p.MarkPrice - p.EntryPrice) / p.EntryPrice * 100
			} else {
				pnlPct = (p.EntryPrice - p.MarkPrice) / p.EntryPrice * 100
			}
		}
		weight, targetWeight, targetQty := 0.0, 0.0, 0.0
		if s.Account.Equity != 0 {
			weight = q * p.MarkPrice / s.Account.Equity * 100
			targetWeight = targets[p.Asset] / s.Account.Equity * 100
		}
		if p.MarkPrice > 0 {
			targetQty = targets[p.Asset] / p.MarkPrice
		}
		fees, funding, realized, openedAt := 0.0, 0.0, 0.0, ""
		if lot := lots[p.Asset]; lot != nil {
			fees, funding, realized, openedAt = lot.Fees, lot.Funding, lot.GrossPnL, tradeTimeLabel(lot.OpenedAt)
		}
		out = append(out, map[string]any{"openedAt": openedAt, "feesUsd": fees, "fundingUsd": funding,
			"netPnlUsd": p.UnrealizedPnL + realized - fees + funding, "feesAvailable": true, "fundingAvailable": true,
			"asset": p.Asset, "side": side, "quantity": math.Abs(q), "quantityLabel": formatQty(math.Abs(q)), "entryPrice": p.EntryPrice, "entryPriceLabel": formatPrice(p.EntryPrice), "currentPrice": p.MarkPrice, "currentPriceLabel": formatPrice(p.MarkPrice), "pnlUsd": p.UnrealizedPnL, "pnlUsdLabel": formatSignedUSD(p.UnrealizedPnL), "pnlPct": pnlPct, "pnlPctLabel": fmt.Sprintf("%+.2f%%", pnlPct), "currentWeightPct": weight, "targetWeightPct": targetWeight, "targetQty": targetQty, "notionalUsd": q * p.MarkPrice, "effectiveQty": q, "localQty": q, "exchangeQty": q, "deltaQty": 0.0, "status": alignmentFromRecon(s.Reconciliation.State), "valuationAvailable": true, "targetAvailable": targetsAvailable, "pnlAvailable": true, "reconciliationAvailable": true, "strategyBreakdown": []map[string]any{}})
	}
	return out
}
