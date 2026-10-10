package provider

import (
	"bufio"
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"sort"
	"strconv"
	"strings"
	"time"
)

const simulationSnapshotVersion = 1
const simulationManualContractVersion = "step58-simulation-manual-v1"

type SimulationConfig struct {
	Dir string
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
	Label  string  `json:"label"`
	Equity float64 `json:"equity"`
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

type simulationManualState struct {
	Ready              bool   `json:"ready"`
	Blocker            string `json:"blocker"`
	DecisionTimestamp  uint64 `json:"decisionTimestamp"`
	ExecutionTimestamp uint64 `json:"executionTimestamp"`
	LastCorrelationID  string `json:"lastCorrelationId"`
	LastStatus         string `json:"lastStatus"`
	LastDetail         string `json:"lastDetail"`
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
}

func NewSimulation(cfg SimulationConfig) *Simulation {
	return &Simulation{cfg: cfg}
}

func (p *Simulation) snapshotPath() string {
	return filepath.Join(p.cfg.Dir, "state.json")
}

func (p *Simulation) load() (simulationSnapshot, error) {
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
		"dataHealthy": s.Phase != "ERROR", "dataState": "VALID", "dataDetail": fmt.Sprintf("historical %s Â· generation %d", s.HistoricalDate, s.Generation),
		"tradingEnabled": s.RouteSafe, "tradingState": map[bool]string{true: "REPLAY ROUTE SAFE", false: "REPLAY PAUSED"}[s.RouteSafe],
		"tradingDetail":  "New exposure is gated by Step54/55 reconciliation; this is MOCK only.",
		"reconciliation": recon, "readiness": readiness, "alertCount": alerts,
		"criticalAlertCount": critical, "warningAlertCount": warnings,
		"utcLabel": fmt.Sprintf("BUSINESS %d Â· %s", s.BusinessTimestamp, s.HistoricalDate),
		"blockers": reconciliationMessages(s), "warnings": []string{}, "sourceMode": "MOCK",
	}
}

func (p *Simulation) overview(s simulationSnapshot) map[string]any {
	positions := dashboardPositions(s)
	unrealized := totalUnrealized(s)
	stats := []map[string]any{
		{"label": "Total Equity", "value": formatUSD2(s.Account.Equity), "detail": "Canonical Step52 MOCK account"},
		{"label": "Cash", "value": formatUSD2(s.Account.CashTotal), "detail": "Settled cash"},
		{"label": "Unrealized PnL", "value": formatSignedUSD(unrealized), "detail": "Canonical position marks"},
		{"label": "Margin Used", "value": formatUSD2(s.Account.MarginUsed), "detail": "Synthetic MOCK margin"},
	}
	curve := make([]map[string]any, 0, len(s.EquityHistory))
	for _, point := range s.EquityHistory {
		curve = append(curve, map[string]any{"label": point.Label, "equity": point.Equity, "benchmark": point.Equity})
	}
	events := []map[string]any{{"time": fmt.Sprintf("%d", s.BusinessTimestamp), "event": fmt.Sprintf("%s Â· day %d/%d", s.Phase, s.DayIndex, s.DayCount), "source": "Step58 replay", "severity": "INFO"}}
	if s.Manual.LastCorrelationID != "" {
		events = append(events, map[string]any{"time": fmt.Sprintf("%d", s.BusinessTimestamp), "event": s.Manual.LastStatus + " Â· " + s.Manual.LastDetail, "source": "Manual Control", "severity": "INFO"})
	}
	return map[string]any{
		"stats": stats, "equityCurve": curve, "liveExpected": []any{}, "positions": positions, "recentEvents": events,
		"checks":     []map[string]any{{"label": "Reconciliation", "state": alignmentFromRecon(s.Reconciliation.State), "detail": fmt.Sprintf("sequence %d/%d", s.Reconciliation.LocalSequence, s.Reconciliation.VenueSequence)}},
		"sourceMode": "MOCK", "sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp),
		"sourceNote":      "Step58 reads canonical MOCK runtime state; no exchange/PG/NATS values are fabricated.",
		"readiness":       map[bool]string{true: "READY", false: "PAUSED"}[s.RouteSafe],
		"readinessDetail": s.Phase, "reconciliationStatus": normalizedReconState(s.Reconciliation.State),
		"equityAvailable": true, "liveExpectedAvailable": false, "marketDataStatus": "ALIGNED",
		"canonicalCycle": fmt.Sprintf("step58-%s-%d", s.HistoricalDate, s.Generation), "sourceWarnings": []string{},
	}
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
	return map[string]any{
		"totalEquity": formatUSD2(s.Account.Equity), "activePositions": len(s.Account.Positions),
		"grossExposure": formatUSD2(gross), "grossExposurePct": fmt.Sprintf("%.2f%%", grossPct),
		"netExposure": formatSignedUSD(net), "unrealizedPnl": formatSignedUSD(totalUnrealized(s)),
		"positions": dashboardPositions(s), "accountCash": formatUSD2(s.Account.CashTotal), "sourceMode": "MOCK",
		"sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp), "valuationState": "AVAILABLE",
		"sourceNote": "Step52 canonical linear-perpetual account snapshot.",
	}
}

func (p *Simulation) reconciliation(s simulationSnapshot) map[string]any {
	rows := make([]map[string]any, 0, len(s.Account.Positions))
	for _, pos := range s.Account.Positions {
		rows = append(rows, map[string]any{
			"asset": pos.Asset, "targetQty": 0.0, "targetQtyLabel": "â€”", "targetAvailable": false,
			"effectiveQty": pos.Quantity, "effectiveQtyLabel": formatQty(pos.Quantity),
			"localQty": pos.Quantity, "localQtyLabel": formatQty(pos.Quantity),
			"exchangeQty": pos.Quantity, "exchangeQtyLabel": formatQty(pos.Quantity), "exchangeAvailable": true,
			"deltaQty": 0.0, "deltaQtyLabel": "0", "pendingQty": 0.0, "pendingQtyLabel": "0",
			"status":      alignmentFromRecon(s.Reconciliation.State),
			"explanation": "Step54 local expected projection compared with recovered MOCK venue truth.",
		})
	}
	open := make([]map[string]any, 0)
	for _, o := range s.Orders {
		if !isOpenStatus(o.Status) {
			continue
		}
		open = append(open, map[string]any{"orderId": o.OrderID, "asset": o.Asset, "side": o.Side, "quantity": o.Quantity, "remaining": o.RemainingQuantity, "age": "business-event", "state": executionOrderState(o.Status), "exchangeId": o.NativeOrderID})
	}
	issues := make([]map[string]any, 0, len(s.Reconciliation.Issues))
	for _, i := range s.Reconciliation.Issues {
		issues = append(issues, map[string]any{"kind": i.Kind, "asset": i.Asset, "orderId": i.OrderID, "localValue": 0, "exchangeValue": 0, "message": i.Message + optionalCompare(i.LocalValue, i.VenueValue)})
	}
	return map[string]any{
		"status": normalizedReconState(s.Reconciliation.State), "lastChecked": fmt.Sprintf("%d", s.BusinessTimestamp), "tolerance": "EXACT FIXED-POINT / explicit identity",
		"targetPortfolioValue": "â€”", "localPortfolioValue": formatUSD2(s.Account.Equity), "exchangePortfolioValue": formatUSD2(s.Account.Equity),
		"rows": rows, "openOrders": open, "sourceMode": "MOCK", "sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp),
		"sourceNote": "Step54 reconciliation. PENDING never becomes CLEAN without same-sequence complete evidence.", "comparisonAvailable": true,
		"exchangeEvidenceTime": fmt.Sprintf("%d", s.BusinessTimestamp), "exchangeEvidenceSequence": s.Reconciliation.VenueSequence, "exchangeSnapshotTimestamp": s.BusinessTimestamp,
		"evidenceFresh": s.Reconciliation.State == "CLEAN", "issues": issues,
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
	fillPriceByOrder := map[string]float64{}
	for _, f := range s.Fills {
		fillPriceByOrder[f.OrderID] = f.Price
	}
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
		avg := "â€”"
		if p, ok := fillPriceByOrder[o.OrderID]; ok {
			avg = formatPrice(p)
		}
		fee := 0.0
		for fillID, amount := range feesByFill {
			_ = fillID
			fee += amount * boolFloat(hasFillForOrder(s, fillID, o.OrderID))
		}
		orders = append(orders, map[string]any{
			"orderId": o.OrderID, "strategyId": o.StrategyID, "exchangeOrderId": o.NativeOrderID, "cycleId": fmt.Sprintf("step58-%s", o.OrderID), "correlationId": fmt.Sprintf("mock-order-%s", o.OrderID),
			"asset": o.Asset, "side": o.Side, "quantity": o.Quantity, "filledQty": o.FilledQuantity, "remainingQty": o.RemainingQuantity, "state": state, "age": "business-event",
			"expectedPriceLabel": formatPrice(o.LimitPrice), "avgFillPriceLabel": avg, "feesLabel": formatUSD2(fee), "slippageBps": 0.0, "slippageBpsLabel": "â€”", "submitLatencyMs": 0, "fillLatencyMs": nil,
			"submittedAt": fmt.Sprintf("%d", o.ActiveFrom), "lastUpdateAt": fmt.Sprintf("%d", s.BusinessTimestamp), "lifecycle": []any{},
		})
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
		"avgSubmitLatencyMs": 0, "avgFillLatencyMs": 0, "avgSlippageBpsLabel": "â€”", "submitLatencyP95Ms": 0, "fillLatencyP95Ms": 0, "bestSlippageBpsLabel": "â€”", "worstSlippageBpsLabel": "â€”",
		"totalFeesLabel": formatUSD2(totalFees), "rejectRateLabel": percent(rejects, len(s.Orders)), "orders": orders, "partialFills": fills, "rejects": []any{}, "replacements": []any{},
		"sourceMode": "MOCK", "sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp), "sourceNote": "Latency/slippage are unavailable in Step58 synthetic OHLCV replay and are not fabricated.", "latencyAvailable": false, "slippageAvailable": false, "orderWindowTruncated": false, "fillWindowTruncated": false,
	}
}

func (p *Simulation) marketData(s simulationSnapshot) map[string]any {
	freshness := make([]map[string]any, 0, len(s.Market))
	for _, b := range s.Market {
		freshness = append(freshness, map[string]any{"asset": b.Asset, "lastCandle": s.HistoricalDate, "ageLabel": "historical replay", "staleThresholdLabel": "event-time sequence", "source": "frozen OHLCV CSV", "state": "HEALTHY"})
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
	return map[string]any{
		"readiness": map[bool]string{true: "READY", false: "PAUSED"}[s.RouteSafe], "readinessReason": "Step58 local simulation read model", "lastUpdated": fmt.Sprintf("%d", s.BusinessTimestamp),
		"vps":        map[string]any{"cpuPct": 0, "ramPct": 0, "diskPct": 0, "load1m": 0, "networkRxLabel": "not sampled", "networkTxLabel": "not sampled", "uptimeLabel": "local simulation", "clockOffsetLabel": "not used for economics", "clockSynced": true, "state": "UNKNOWN"},
		"containers": []any{}, "services": []map[string]any{{"service": "Step58 simulation runner", "ready": s.Phase != "ERROR", "mode": "REPLAY", "lastEvent": fmt.Sprintf("%d", s.BusinessTimestamp), "lagLabel": "generation-driven", "health": state}},
		"postgres":     map[string]any{"connected": false, "latencyMs": 0, "activeConnections": 0, "maxConnections": 0, "storageUsedLabel": "not used", "storagePct": 0, "persistenceState": "Step53 file recovery", "state": "UNKNOWN"},
		"nats":         map[string]any{"connected": false, "streams": 0, "consumers": 0, "pendingMessages": 0, "maxConsumerLag": 0, "redeliveries": 0, "ackHealthLabel": "not used in local Step58 simulation bridge", "state": "UNKNOWN"},
		"exchange":     map[string]any{"venue": "MOCK", "connected": s.Phase != "ERROR", "reconnectState": "DETERMINISTIC", "lastApiActivity": fmt.Sprintf("%d", s.BusinessTimestamp), "lastWsActivity": fmt.Sprintf("%d", s.BusinessTimestamp), "apiLatencyMs": 0, "state": state},
		"outbox":       map[string]any{"pendingMessages": 0, "oldestAgeLabel": "0", "lastPublished": "file generation", "state": "HEALTHY"},
		"dependencies": []map[string]any{{"component": "Step53 recovery", "state": state, "reason": "canonical MOCK durable state", "lastCheck": fmt.Sprintf("%d", s.BusinessTimestamp)}, {"component": "Step54 reconciliation", "state": map[string]string{"CLEAN": "HEALTHY", "PENDING": "WARN", "BLOCKED": "CRITICAL"}[s.Reconciliation.State], "reason": s.Reconciliation.State, "lastCheck": fmt.Sprintf("%d", s.BusinessTimestamp)}},
		"sourceMode":   "MOCK", "sourceNote": "Host/container telemetry is intentionally not fabricated by the simulation read-model provider.",
	}
}

func (p *Simulation) alertsAudit(s simulationSnapshot) map[string]any {
	alerts := []map[string]any{}
	if s.Reconciliation.State != "CLEAN" {
		sev := "WARN"
		if s.Reconciliation.State == "BLOCKED" {
			sev = "CRITICAL"
		}
		alerts = append(alerts, map[string]any{"id": fmt.Sprintf("recon-%d", s.Generation), "timestamp": fmt.Sprintf("%d", s.BusinessTimestamp), "severity": sev, "status": "ACTIVE", "service": "Reconciliation", "eventType": "RECONCILIATION_" + s.Reconciliation.State, "title": "MOCK reconciliation is " + s.Reconciliation.State, "detail": strings.Join(reconciliationMessages(s), "; "), "correlationId": fmt.Sprintf("step58-%d", s.Generation)})
	}
	audits := []map[string]any{{"id": fmt.Sprintf("step58-%d", s.Generation), "timestamp": fmt.Sprintf("%d", s.BusinessTimestamp), "actor": "system", "actorType": "SYSTEM", "action": "SIMULATION_SNAPSHOT", "target": "MOCK", "result": "SUCCESS", "correlationId": fmt.Sprintf("step58-generation-%d", s.Generation), "detail": fmt.Sprintf("%s %s", s.Phase, s.HistoricalDate)}}
	if s.Manual.LastCorrelationID != "" {
		audits = append([]map[string]any{{"id": s.Manual.LastCorrelationID, "timestamp": fmt.Sprintf("%d", s.BusinessTimestamp), "actor": "operator", "actorType": "HUMAN", "action": "MANUAL_ROUTE_SIMULATION", "target": "portfolio", "result": manualAuditResult(s.Manual.LastStatus), "correlationId": s.Manual.LastCorrelationID, "detail": s.Manual.LastDetail}}, audits...)
	}
	critical, warn := 0, 0
	for _, a := range alerts {
		if a["severity"] == "CRITICAL" {
			critical++
		} else if a["severity"] == "WARN" {
			warn++
		}
	}
	return map[string]any{"activeCritical": critical, "activeWarnings": warn, "acknowledged": 0, "resolved24h": 0, "alerts": alerts, "audit": audits, "sourceMode": "MOCK", "sourceUpdatedAt": fmt.Sprintf("%d", s.BusinessTimestamp), "sourceNote": "Simulation alerts are derived from canonical reconciliation/manual outcomes only.", "acknowledgementAvailable": false, "durableAlertHistoryAvailable": false, "humanAuditAvailable": true, "auditMode": "SIMULATION_CANONICAL_EVIDENCE", "watchdogAvailable": false, "watchdogState": "NOT_WIRED", "durableEventCount": 0, "durableLifecycleEvents": []any{}, "acknowledgementEventCount": 0}
}

func (p *Simulation) liveVsExpected(s simulationSnapshot) map[string]any {
	return map[string]any{"contractVersion": "step58-simulation-v1", "status": "INSUFFICIENT_DATA", "validated": true, "projectionReady": false, "observationMode": "MOCK_REPLAY", "baselineLabel": "Not fabricated", "baselineWindow": "Step59 will validate deterministic baselines/speed invariance", "baselineFingerprint": "", "baselineObservationCount": 0, "baselineExcludesLatest": true, "latestObservation": s.HistoricalDate, "metricCount": 0, "normalCount": 0, "elevatedCount": 0, "abnormalCount": 0, "criticalCount": 0, "overallClassification": "INSUFFICIENT_DATA", "anomalyCount": 0, "anomalies": []any{}, "coverage": []map[string]any{{"id": "execution", "label": "Execution/accounting baseline", "state": "DEFERRED", "source": "Step59", "detail": "Step58 exposes live canonical values but does not fabricate an accepted statistical baseline."}}, "metrics": []any{}, "sourceMode": "MOCK", "sourceNote": "No baseline classification is invented.", "readOnly": true, "privateAuth": "DISABLED", "orderRouting": "MOCK_ONLY", "checkedAt": fmt.Sprintf("%d", s.BusinessTimestamp)}
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
	for _, e := range s.Ledger.Entries {
		if e.Kind != "FILL" {
			continue
		}
		commission := feeByFill[e.NativeFillID]
		row := LedgerEntry{EntryID: e.EntryID, FillID: e.NativeFillID, OrderID: e.OrderID, StrategyID: e.StrategyID, Timestamp: fmt.Sprintf("%d", e.EventTime), Asset: e.Asset, Side: e.Side, Quantity: math.Abs(e.PositionDelta), Price: 0, GrossNotional: e.GrossNotional, Commission: commission, PositionDelta: e.PositionDelta, CashDelta: e.CashDelta + cashAccountingByFill[e.NativeFillID], EntryHash: e.EntryHash}
		if f, ok := findFill(s, e.NativeFillID); ok {
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

func (p *Simulation) VenueFoundation(_ context.Context) VenueFoundation {
	return VenueFoundation{Status: "MOCK_REPLAY_READY", FoundationReady: true, Venue: "MOCK", TargetEnvironment: "MOCK", GatewayMode: "canonical-adapter", PublicConnectivity: "IN_PROCESS", PrivateAuth: "DISABLED", OrderRouting: "MOCK_ONLY", SymbolMapping: "EXPLICIT_STEP49", ExchangeFilters: "STEP49_RULES", SecretsRequired: false, CapitalRequired: false, ReadOnly: true, SourceContract: "CanonicalVenueAdapter -> MockExchangeAdapterV1", Note: "Step58 simulation venue only; no Hyperliquid/private credentials or capital."}
}

func (p *Simulation) PreviewManualControl(_ context.Context, req ManualControlPreviewRequest) (ManualControlData, error) {
	s, err := p.load()
	if err != nil {
		return ManualControlData{}, err
	}
	return p.manualPreview(s, req)
}

type SimulationManualRouteRequest struct {
	Actor                    string
	Filename                 string
	CSV                      string
	RequestHash              string
	ReferenceTargetTimestamp string
}

type SimulationManualRouteResult struct {
	ContractVersion          string   `json:"contractVersion"`
	Status                   string   `json:"status"`
	Submitted                bool     `json:"submitted"`
	RouteEnabled             bool     `json:"routeEnabled"`
	ConfirmationAccepted     bool     `json:"confirmationAccepted"`
	RequestHash              string   `json:"requestHash"`
	CorrelationID            string   `json:"correlationId"`
	Actor                    string   `json:"actor"`
	ReferenceTargetTimestamp string   `json:"referenceTargetTimestamp"`
	CheckedAt                string   `json:"checkedAt"`
	Blockers                 []string `json:"blockers"`
	AuditPersisted           bool     `json:"auditPersisted"`
	Note                     string   `json:"note"`
}

func (p *Simulation) RouteManualControl(_ context.Context, req SimulationManualRouteRequest) (SimulationManualRouteResult, error) {
	s, err := p.load()
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	preview, err := p.manualPreview(s, ManualControlPreviewRequest{Actor: req.Actor, Filename: req.Filename, CSV: req.CSV})
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	result := SimulationManualRouteResult{ContractVersion: simulationManualContractVersion, Status: "BLOCKED", Submitted: false, RouteEnabled: preview.RouteEnabled, ConfirmationAccepted: true, RequestHash: preview.RequestHash, Actor: req.Actor, ReferenceTargetTimestamp: preview.CurrentTargetTimestamp, CheckedAt: time.Now().UTC().Format(time.RFC3339Nano), Blockers: append([]string(nil), preview.RouteBlockers...), AuditPersisted: false}
	if req.RequestHash == "" || req.RequestHash != preview.RequestHash {
		result.Blockers = append(result.Blockers, "REQUEST_HASH_MISMATCH")
	}
	if req.ReferenceTargetTimestamp == "" || req.ReferenceTargetTimestamp != preview.CurrentTargetTimestamp {
		result.Blockers = append(result.Blockers, "STALE_REFERENCE_TARGET")
	}
	result.Blockers = uniqueStrings(result.Blockers)
	if !preview.ValidationPassed || !preview.RouteEnabled || len(result.Blockers) > 0 {
		result.Note = "Step58 simulation route rejected fail-closed before the trading-control file transport."
		return result, nil
	}
	correlation, err := simulationCorrelationID()
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	result.CorrelationID = correlation
	targets, cash, err := parseSimulationManualCSV(req.CSV)
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	requestsDir := filepath.Join(p.cfg.Dir, "requests")
	if err := os.MkdirAll(requestsDir, 0o700); err != nil {
		return SimulationManualRouteResult{}, err
	}
	temp, err := os.CreateTemp(requestsDir, ".manual-*.tmp")
	if err != nil {
		return SimulationManualRouteResult{}, err
	}
	writer := bufio.NewWriter(temp)
	fmt.Fprintln(writer, "STEP58_MANUAL_V1")
	fmt.Fprintf(writer, "request_id=%s\n", correlation)
	fmt.Fprintf(writer, "correlation_id=%s\n", correlation)
	fmt.Fprintf(writer, "actor=%s\n", sanitizeLine(req.Actor))
	fmt.Fprintf(writer, "request_hash=%s\n", preview.RequestHash)
	fmt.Fprintf(writer, "decision_timestamp=%d\n", s.Manual.DecisionTimestamp)
	fmt.Fprintf(writer, "execution_timestamp=%d\n", s.Manual.ExecutionTimestamp)
	fmt.Fprintf(writer, "reference_generation=%d\n", s.Generation)
	keys := make([]string, 0, len(targets))
	for asset := range targets {
		keys = append(keys, asset)
	}
	sort.Strings(keys)
	for _, asset := range keys {
		fmt.Fprintf(writer, "target=%s,%.12f\n", asset, targets[asset]/100.0)
	}
	fmt.Fprintf(writer, "cash=%.12f\n", cash/100.0)
	if err := writer.Flush(); err != nil {
		temp.Close()
		os.Remove(temp.Name())
		return SimulationManualRouteResult{}, err
	}
	if err := temp.Sync(); err != nil {
		temp.Close()
		os.Remove(temp.Name())
		return SimulationManualRouteResult{}, err
	}
	if err := temp.Close(); err != nil {
		os.Remove(temp.Name())
		return SimulationManualRouteResult{}, err
	}
	final := filepath.Join(requestsDir, correlation+".request")
	if err := os.Rename(temp.Name(), final); err != nil {
		os.Remove(temp.Name())
		return SimulationManualRouteResult{}, err
	}
	result.Status = "ACCEPTED"
	result.AuditPersisted = true
	result.Note = "Confirmed MOCK request was durably queued to the Step58 trading-control file transport. The runner will invoke the Step57 manual Risk -> Planner -> CanonicalVenueAdapter pipeline; no browser-to-venue path exists."
	return result, nil
}

func (p *Simulation) manualBase(s simulationSnapshot, actor string) (ManualControlData, error) {
	example := "asset,weight_pct\nBTCUSDT,10\nCASH,90\n"
	blockers := []string{}
	if !s.Manual.Ready {
		b := s.Manual.Blocker
		if b == "" {
			b = "SIMULATION_MANUAL_SINK_NOT_READY"
		}
		blockers = append(blockers, b)
	}
	if s.Reconciliation.State != "CLEAN" {
		blockers = append(blockers, "RECONCILIATION_NOT_CLEAN")
	}
	if !s.RouteSafe {
		blockers = append(blockers, "ROUTE_SAFETY_FALSE")
	}
	return ManualControlData{ContractVersion: simulationManualContractVersion, RoutingContractReady: s.Manual.Ready, RoutingContractMode: "MOCK_SIMULATION_NORMAL_PIPELINE", ExecutionBoundary: "OPERATOR -> dashboard-api -> durable simulation request -> Step57 ManualPortfolioRisk -> production Planner -> CanonicalVenueAdapter -> MOCK", ExchangeConstraintsValidated: s.Manual.Ready, TradingControlSink: "STEP58_DURABLE_FILE_TRANSPORT", PrivateAuth: "DISABLED", OrderLifecycle: "STEP50_TO_STEP55_CANONICAL_MOCK", ConfirmationRequired: true, ConfirmationPhrase: ManualControlConfirmationPhrase, HumanAuditAvailable: true, RouteBlockers: uniqueStrings(blockers), RecentRouteAudits: []ManualRouteAuditRow{}, Mode: "REPLAY", Exchange: "MOCK / NORMAL PIPELINE", SchemaLabel: "asset,weight_pct", MaxUploadSizeLabel: "256 KB", ExampleCSV: example, RequestHash: "", PreviewRows: []ManualPreviewRow{}, ValidationIssues: []ManualValidationIssue{{Severity: "INFO", Field: "backend", Message: "Simulation preview is server-side; confirmed routes are queued to the Step57 normal pipeline only when the replay has reached MANUAL_READY and reconciliation is CLEAN."}}, OrderPreview: []ManualOrderPreviewRow{}, EstimatedFeesLabel: "Step52 canonical fee on fill", EstimatedTurnoverLabel: "â€”", AuditActorLabel: actor, BackendAuthoritative: true, ValidationPassed: false, RiskCheckAvailable: s.Manual.Ready, RouteEnabled: s.Manual.Ready && s.Reconciliation.State == "CLEAN" && s.RouteSafe, SourceMode: "MOCK", CurrentTargetTimestamp: strconv.FormatUint(s.BusinessTimestamp, 10), PreviewKind: "MOCK_TARGET_DELTA", SafetyNote: "Step58 enables manual routing only inside the MOCK simulation sink after the historical replay has handed off a flat, CLEAN account. REAL/private routing remains disabled."}, nil
}

func (p *Simulation) manualPreview(s simulationSnapshot, req ManualControlPreviewRequest) (ManualControlData, error) {
	result, err := p.manualBase(s, req.Actor)
	if err != nil {
		return ManualControlData{}, err
	}
	targets, cash, parseErr := parseSimulationManualCSV(req.CSV)
	if parseErr != nil {
		result.ValidationIssues = []ManualValidationIssue{{Severity: "ERROR", Field: "csv", Message: parseErr.Error()}}
		result.RouteEnabled = false
		return result, nil
	}
	routable := map[string]struct{}{}
	for _, a := range s.RoutableAssets {
		routable[a] = struct{}{}
	}
	issues := []ManualValidationIssue{}
	for asset := range targets {
		if _, ok := routable[asset]; !ok {
			issues = append(issues, ManualValidationIssue{Severity: "ERROR", Field: "asset", Message: asset + " has no current historical price / explicit MOCK route in this simulation handoff"})
		}
	}
	if len(issues) == 0 {
		issues = append(issues, ManualValidationIssue{Severity: "INFO", Field: "csv", Message: "Schema, weights, current-price availability and explicit MOCK routability validated. Step57 performs the authoritative manual risk transformation at execution."})
	}
	current := currentWeights(s)
	assets := map[string]struct{}{"CASH": {}}
	for a := range targets {
		assets[a] = struct{}{}
	}
	for a := range current {
		assets[a] = struct{}{}
	}
	names := make([]string, 0, len(assets))
	for a := range assets {
		names = append(names, a)
	}
	sort.Strings(names)
	rows := []ManualPreviewRow{}
	orders := []ManualOrderPreviewRow{}
	turnover := 0.0
	for _, a := range names {
		requested := cash
		if a != "CASH" {
			requested = targets[a]
		}
		cur := current[a]
		delta := requested - cur
		turnover += math.Abs(delta)
		estimated := math.Abs(delta) / 100.0 * s.Account.Equity
		rows = append(rows, ManualPreviewRow{Asset: a, CurrentWeightPct: cur, RequestedWeightPct: requested, ApprovedWeightPct: requested, EstimatedNotionalLabel: formatUSD2(estimated), DeltaPct: delta, RiskNote: "Step57 risk executes at route time"})
		action := "HOLD"
		if a != "CASH" {
			if delta > 1e-9 {
				action = "BUY"
			} else if delta < -1e-9 {
				action = "SELL"
			}
		}
		if a != "CASH" {
			orders = append(orders, ManualOrderPreviewRow{Asset: a, Action: action, DeltaWeightPct: delta, EstimatedNotionalLabel: formatUSD2(estimated), EstimatedNotionalUSD: estimated, EstimatedFeeLabel: "canonical on fill", Note: "Step57 normal pipeline; not browser-executable"})
		}
	}
	canonical := canonicalManualTargets(targets, cash)
	sum := sha256.Sum256([]byte(canonical))
	result.RequestHash = "sha256:" + hex.EncodeToString(sum[:])
	result.PreviewRows = rows
	result.OrderPreview = orders
	result.ValidationIssues = issues
	result.ValidationPassed = len(issues) == 1 && issues[0].Severity == "INFO"
	result.ExchangeConstraintsValidated = result.ValidationPassed && s.Manual.Ready
	result.EstimatedTurnoverLabel = fmt.Sprintf("%.2f%%", turnover/2.0)
	result.CurrentTargetTimestamp = strconv.FormatUint(s.BusinessTimestamp, 10)
	if !result.ValidationPassed {
		result.RouteEnabled = false
		result.RouteBlockers = uniqueStrings(append(result.RouteBlockers, "PREVIEW_VALIDATION_FAILED"))
	}
	return result, nil
}

func parseSimulationManualCSV(raw string) (map[string]float64, float64, error) {
	lines := strings.Split(strings.ReplaceAll(raw, "\r\n", "\n"), "\n")
	if len(lines) < 2 || strings.TrimSpace(lines[0]) != "asset,weight_pct" {
		return nil, 0, errors.New("expected CSV header asset,weight_pct")
	}
	targets := map[string]float64{}
	seen := map[string]struct{}{}
	cash := 0.0
	total := 0.0
	for _, line := range lines[1:] {
		line = strings.TrimSpace(line)
		if line == "" {
			continue
		}
		parts := strings.Split(line, ",")
		if len(parts) != 2 {
			return nil, 0, errors.New("each CSV row must contain exactly asset,weight_pct")
		}
		asset := strings.ToUpper(strings.TrimSpace(parts[0]))
		if asset == "" {
			return nil, 0, errors.New("asset cannot be empty")
		}
		if _, dup := seen[asset]; dup {
			return nil, 0, fmt.Errorf("duplicate asset %s", asset)
		}
		seen[asset] = struct{}{}
		weight, err := strconv.ParseFloat(strings.TrimSpace(parts[1]), 64)
		if err != nil || !finiteSimulation(weight) || weight < 0 || weight > 100 {
			return nil, 0, fmt.Errorf("invalid weight for %s", asset)
		}
		total += weight
		if asset == "CASH" {
			cash = weight
		} else {
			targets[asset] = weight
		}
	}
	if math.Abs(total-100.0) > 1e-9 {
		return nil, 0, fmt.Errorf("weights must sum to 100%%, got %.12f", total)
	}
	return targets, cash, nil
}

func canonicalManualTargets(targets map[string]float64, cash float64) string {
	keys := make([]string, 0, len(targets))
	for k := range targets {
		keys = append(keys, k)
	}
	sort.Strings(keys)
	var b strings.Builder
	for _, k := range keys {
		fmt.Fprintf(&b, "%s=%.12f\n", k, targets[k])
	}
	fmt.Fprintf(&b, "CASH=%.12f\n", cash)
	return b.String()
}
func simulationCorrelationID() (string, error) {
	var raw [12]byte
	if _, err := rand.Read(raw[:]); err != nil {
		return "", err
	}
	return "step58-manual-" + hex.EncodeToString(raw[:]), nil
}
func sanitizeLine(v string) string {
	return strings.ReplaceAll(strings.ReplaceAll(v, "\n", " "), "\r", " ")
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
		return "â€”"
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
func boolFloat(v bool) float64 {
	if v {
		return 1
	}
	return 0
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
func hasFillForOrder(s simulationSnapshot, fillID, orderID string) bool {
	for _, f := range s.Fills {
		if f.FillID == fillID && f.OrderID == orderID {
			return true
		}
	}
	return false
}
func findFill(s simulationSnapshot, id string) (simulationFill, bool) {
	for _, f := range s.Fills {
		if f.FillID == id {
			return f, true
		}
	}
	return simulationFill{}, false
}
func manualAuditResult(status string) string {
	if status == "SUBMITTED" || status == "NOOP" {
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
		weight := 0.0
		if s.Account.Equity != 0 {
			weight = q * p.MarkPrice / s.Account.Equity * 100
		}
		out = append(out, map[string]any{"asset": p.Asset, "side": side, "quantity": math.Abs(q), "quantityLabel": formatQty(math.Abs(q)), "entryPrice": p.EntryPrice, "entryPriceLabel": formatPrice(p.EntryPrice), "currentPrice": p.MarkPrice, "currentPriceLabel": formatPrice(p.MarkPrice), "pnlUsd": p.UnrealizedPnL, "pnlUsdLabel": formatSignedUSD(p.UnrealizedPnL), "pnlPct": pnlPct, "pnlPctLabel": fmt.Sprintf("%+.2f%%", pnlPct), "currentWeightPct": weight, "targetWeightPct": 0.0, "targetQty": 0.0, "effectiveQty": q, "localQty": q, "exchangeQty": q, "deltaQty": 0.0, "status": alignmentFromRecon(s.Reconciliation.State), "valuationAvailable": true, "targetAvailable": false, "pnlAvailable": true, "reconciliationAvailable": true, "strategyBreakdown": []map[string]any{}})
	}
	return out
}
