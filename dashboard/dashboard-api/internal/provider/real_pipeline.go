package provider

import (
	"context"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

type realPipelineData struct {
	Proof           *realEndToEndProofData  `json:"proof,omitempty"`
	CycleLabel      string                  `json:"cycleLabel"`
	LatestDecision  string                  `json:"latestDecision"`
	UniverseSize    int                     `json:"universeSize"`
	ActiveSignals   int                     `json:"activeSignals"`
	ActionablePlans int                     `json:"actionablePlans"`
	Rows            []realPipelineAssetRow  `json:"rows"`
	Traces          map[string]realWhyTrace `json:"traces"`
	SourceMode      string                  `json:"sourceMode"`
	SourceUpdatedAt string                  `json:"sourceUpdatedAt"`
	SourceNote      string                  `json:"sourceNote"`
}

type realPipelineStage struct {
	ID     string `json:"id"`
	Label  string `json:"label"`
	Value  string `json:"value"`
	Detail string `json:"detail"`
	State  string `json:"state"`
}

type realPipelineAssetRow struct {
	CycleID             string              `json:"cycleId"`
	Asset               string              `json:"asset"`
	DecisionTime        string              `json:"decisionTime"`
	Rank                int                 `json:"rank"`
	RankLabel           string              `json:"rankLabel"`
	RSI                 float64             `json:"rsi"`
	RSILabel            string              `json:"rsiLabel"`
	Signal              string              `json:"signal"`
	RawTargetPct        float64             `json:"rawTargetPct"`
	RawTargetLabel      string              `json:"rawTargetLabel"`
	VolTargetPct        float64             `json:"volTargetPct"`
	VolTargetLabel      string              `json:"volTargetLabel"`
	ApprovedTargetPct   float64             `json:"approvedTargetPct"`
	ApprovedTargetLabel string              `json:"approvedTargetLabel"`
	CurrentWeightPct    float64             `json:"currentWeightPct"`
	CurrentWeightLabel  string              `json:"currentWeightLabel"`
	RequiredDeltaPct    float64             `json:"requiredDeltaPct"`
	RequiredDeltaLabel  string              `json:"requiredDeltaLabel"`
	PlannedAction       string              `json:"plannedAction"`
	ExchangeState       string              `json:"exchangeState"`
	Stages              []realPipelineStage `json:"stages"`
}

type realWhyTrace struct {
	CycleID        string                `json:"cycleId"`
	Asset          string                `json:"asset"`
	Market         realWhyMarket         `json:"market"`
	Strategy       realWhyStrategy       `json:"strategy"`
	Portfolio      realWhyPortfolio      `json:"portfolio"`
	Planning       realWhyPlanning       `json:"planning"`
	Execution      realWhyExecution      `json:"execution"`
	Reconciliation realWhyReconciliation `json:"reconciliation"`
	Audit          realWhyAudit          `json:"audit"`
}

type realWhyMarket struct {
	Price         string `json:"price"`
	Universe      string `json:"universe"`
	LiquidityRank string `json:"liquidityRank"`
	Freshness     string `json:"freshness"`
}
type realWhyStrategy struct {
	RSI        string `json:"rsi"`
	Signal     string `json:"signal"`
	Transition string `json:"transition"`
	Intent     string `json:"intent"`
}
type realWhyPortfolio struct {
	RawTarget      string   `json:"rawTarget"`
	VolTarget      string   `json:"volTarget"`
	RiskChanges    []string `json:"riskChanges"`
	ApprovedTarget string   `json:"approvedTarget"`
}
type realWhyPlanning struct {
	EffectiveQty  string `json:"effectiveQty"`
	PendingQty    string `json:"pendingQty"`
	Delta         string `json:"delta"`
	StateRevision string `json:"stateRevision"`
}
type realWhyExecution struct {
	OrderIDs  []string `json:"orderIds"`
	Side      string   `json:"side"`
	Quantity  string   `json:"quantity"`
	FillState string   `json:"fillState"`
	Fees      string   `json:"fees"`
	Slippage  string   `json:"slippage"`
}
type realWhyReconciliation struct {
	LocalQty    string `json:"localQty"`
	ExchangeQty string `json:"exchangeQty"`
	Status      string `json:"status"`
}
type realWhyAudit struct {
	CorrelationID      string `json:"correlationId"`
	ConfigVersion      string `json:"configVersion"`
	Mode               string `json:"mode"`
	Exchange           string `json:"exchange"`
	DecisionTimestamp  string `json:"decisionTimestamp"`
	ExecutionTimestamp string `json:"executionTimestamp"`
}

type pipelineAssetFacts struct {
	asset           string
	signal          string
	signalDetail    string
	targetNotional  float64
	currentQty      float64
	currentNotional float64
	pendingQty      float64
	deltaNotional   float64
	plannedOrders   []tradingwire.PlannedNotionalOrder
	plannedAction   string
	exchangeState   string
	exchangeStage   string
	exchangeDetail  string
	referenceClose  float64
}

func (p *Real) pipeline(ctx context.Context) (realPipelineData, error) {
	checkpoint, err := p.postgres.LatestPipelineCheckpoint(ctx)
	if err != nil {
		return realPipelineData{}, fmt.Errorf("read durable pipeline checkpoint: %w", err)
	}
	state, err := p.postgres.RuntimeState(ctx)
	if err != nil {
		return realPipelineData{}, fmt.Errorf("read durable execution state for pipeline: %w", err)
	}

	recon, reconErr := p.reconciliation(ctx)
	data := buildRealPipeline(checkpoint, state, recon, reconErr, p.runtimeMode())

	var fills pgstore.FillWindow
	var fillsErr error
	if checkpoint.Plan != nil && len(checkpoint.Plan.SubmitOrders) > 0 {
		orderIDs := make([]uint64, 0, len(checkpoint.Plan.SubmitOrders))
		for _, order := range checkpoint.Plan.SubmitOrders {
			orderIDs = append(orderIDs, order.OrderID)
		}
		fills, fillsErr = p.postgres.FillsForOrders(ctx, orderIDs, 2000)
	}
	proof := buildRealEndToEndProof(checkpoint, state, recon, reconErr, fills, fillsErr, p.runtimeMode())
	data.Proof = &proof
	return data, nil
}

func buildRealPipeline(checkpoint pgstore.PipelineCheckpoint, state pgstore.RuntimeState, reconciliation realReconciliationData, reconciliationErr error, mode string) realPipelineData {
	assets := map[string]struct{}{}
	activeSignals := 0
	for _, strategy := range checkpoint.Signals.Strategies {
		for coin, signal := range strategy.Signals {
			assets[coin] = struct{}{}
			if math.Abs(signal) > 1e-12 {
				activeSignals++
			}
		}
	}
	for _, strategy := range checkpoint.Decision.Strategies {
		for coin := range strategy.TargetNotionalUSD {
			assets[coin] = struct{}{}
		}
		for _, decision := range strategy.Decisions {
			assets[decision.Coin] = struct{}{}
		}
	}
	for coin := range checkpoint.Account.Positions {
		assets[coin] = struct{}{}
	}

	currentQty := map[string]float64{}
	currentNotional := map[string]float64{}
	pendingQty := map[string]float64{}
	referenceCloses := map[string]float64{}
	if checkpoint.PlanningRequest != nil {
		for coin, close := range checkpoint.PlanningRequest.ReferenceCloses.Closes {
			referenceCloses[coin] = close
		}
		for _, strategy := range checkpoint.PlanningRequest.State.StrategyPositions {
			for coin, qty := range strategy.Positions {
				assets[coin] = struct{}{}
				currentQty[coin] += qty
				currentNotional[coin] += qty * referenceCloses[coin]
			}
		}
		for _, order := range checkpoint.PlanningRequest.State.Orders {
			if order.Status == 4 || order.Status == 5 || order.Status == 6 {
				continue
			}
			remaining := math.Max(0, order.Request.Quantity-order.FilledQuantity)
			if order.Request.Side == 1 {
				remaining = -remaining
			}
			pendingQty[order.Request.Coin] += remaining
			assets[order.Request.Coin] = struct{}{}
		}
	}

	targetNotional := map[string]float64{}
	for _, strategy := range checkpoint.Decision.Strategies {
		for coin, value := range strategy.TargetNotionalUSD {
			targetNotional[coin] += value
		}
	}
	plannedByCoin := map[string][]tradingwire.PlannedNotionalOrder{}
	if checkpoint.Plan != nil {
		for coin, value := range checkpoint.Plan.GlobalTargetNotionalUSD {
			targetNotional[coin] = value
			assets[coin] = struct{}{}
		}
		for _, order := range checkpoint.Plan.SubmitOrders {
			plannedByCoin[order.Coin] = append(plannedByCoin[order.Coin], order)
			assets[order.Coin] = struct{}{}
		}
	}

	runtimeByID := map[uint64]tradingwire.PersistedTrackedOrder{}
	for _, order := range state.Snapshot.Orders {
		runtimeByID[order.OrderID] = order
	}
	reconByAsset := map[string]realReconciliationRow{}
	if reconciliationErr == nil {
		for _, row := range reconciliation.Rows {
			reconByAsset[row.Asset] = row
		}
	}

	orderedAssets := make([]string, 0, len(assets))
	for coin := range assets {
		if strings.TrimSpace(coin) != "" {
			orderedAssets = append(orderedAssets, coin)
		}
	}
	sort.Strings(orderedAssets)

	rows := make([]realPipelineAssetRow, 0, len(orderedAssets))
	traces := make(map[string]realWhyTrace, len(orderedAssets))
	for _, coin := range orderedAssets {
		facts := pipelineFactsForAsset(coin, checkpoint, state, targetNotional, currentQty, currentNotional, pendingQty, plannedByCoin[coin], runtimeByID, referenceCloses)
		cycleID := fmt.Sprintf("cycle-%s-%d", strings.ToLower(coin), checkpoint.Timestamp)
		row := pipelineRow(checkpoint, facts, cycleID)
		rows = append(rows, row)
		traces[cycleID] = pipelineTrace(checkpoint, state, facts, cycleID, reconByAsset[coin], reconciliationErr, mode)
	}

	universe := len(orderedAssets)
	if checkpoint.StrategyUpdate != nil && checkpoint.StrategyUpdate.ActiveTopN > 0 {
		universe = int(checkpoint.StrategyUpdate.ActiveTopN)
	}
	actionable := 0
	if checkpoint.Plan != nil {
		actionable = len(checkpoint.Plan.SubmitOrders) + len(checkpoint.Plan.CancelOrderIDs)
	}
	plannerState := "Planner checkpoint is present for the same decision timestamp."
	if checkpoint.Plan == nil || checkpoint.PlanningRequest == nil {
		plannerState = "Planner checkpoint is not present for this decision timestamp; planner stages are shown as PENDING, never inferred."
	}
	return realPipelineData{
		CycleLabel:      fmt.Sprintf("REAL · decision %d", checkpoint.Timestamp),
		LatestDecision:  formatTradingTimestamp(checkpoint.Timestamp),
		UniverseSize:    universe,
		ActiveSignals:   activeSignals,
		ActionablePlans: actionable,
		Rows:            rows,
		Traces:          traces,
		SourceMode:      "REAL",
		SourceUpdatedAt: state.UpdatedAt,
		SourceNote:      "Cycle-aligned lineage anchored on portfolio_risk_live_decision_checkpoint. Strategy intent/account/DecisionBatch are the exact persisted inputs/outputs for that decision. " + plannerState + " RSI, liquidity rank, raw target and intermediate volatility/risk transforms are not persisted in these checkpoints and are explicitly shown as unavailable.",
	}
}

func pipelineFactsForAsset(coin string, checkpoint pgstore.PipelineCheckpoint, state pgstore.RuntimeState, target, currentQty, currentNotional, pending map[string]float64, planned []tradingwire.PlannedNotionalOrder, runtime map[uint64]tradingwire.PersistedTrackedOrder, closes map[string]float64) pipelineAssetFacts {
	signal, detail := aggregateSignal(checkpoint.Signals, coin)
	facts := pipelineAssetFacts{asset: coin, signal: signal, signalDetail: detail, targetNotional: target[coin], currentQty: currentQty[coin], currentNotional: currentNotional[coin], pendingQty: pending[coin], plannedOrders: planned, referenceClose: closes[coin]}
	if facts.currentQty == 0 && checkpoint.PlanningRequest == nil {
		facts.currentQty = checkpoint.Account.Positions[coin]
		facts.currentNotional = facts.currentQty * facts.referenceClose
	}
	if len(planned) == 0 {
		facts.plannedAction = "NO ORDER"
		facts.exchangeState, facts.exchangeStage, facts.exchangeDetail = "NONE", "OK", "No submit order was persisted for this asset in the cycle."
		return facts
	}
	actions := make([]string, 0, len(planned))
	states := make([]string, 0, len(planned))
	qty := 0.0
	for _, order := range planned {
		facts.deltaNotional += order.DeltaNotionalUSD
		actions = append(actions, fmt.Sprintf("%s %s", sideLabel(order.Side), formatUSD(order.NotionalUSD)))
		if tracked, ok := runtime[order.OrderID]; ok {
			states = append(states, mapExecutionState(tracked))
			qty += tracked.Quantity
		} else {
			states = append(states, "PENDING")
		}
	}
	facts.plannedAction = strings.Join(actions, " + ")
	facts.exchangeState, facts.exchangeStage, facts.exchangeDetail = aggregateExecutionStates(states)
	_ = qty
	return facts
}

func pipelineRow(checkpoint pgstore.PipelineCheckpoint, facts pipelineAssetFacts, cycleID string) realPipelineAssetRow {
	plannerReady := checkpoint.Plan != nil && checkpoint.PlanningRequest != nil
	currentLabel := signedQuantity(facts.currentQty)
	if facts.referenceClose > 0 {
		currentLabel += " · " + formatUSD(facts.currentNotional)
	}
	deltaLabel := "No submit delta"
	if len(facts.plannedOrders) > 0 {
		deltaLabel = signedUSD(facts.deltaNotional)
	}
	approved := formatUSD(facts.targetNotional)
	stages := []realPipelineStage{
		{ID: "market", Label: "Market / Universe", Value: priceOrUnavailable(facts.referenceClose), Detail: marketDetail(checkpoint), State: stageState(checkpoint.StrategyUpdate != nil, "OK", "PENDING")},
		{ID: "indicators", Label: "Indicators", Value: "Not persisted", Detail: "RSI(7) and liquidity rank are calculated upstream but are not durable fields in the daily checkpoint.", State: "PENDING"},
		{ID: "signal", Label: "Signal", Value: facts.signal, Detail: facts.signalDetail, State: "OK"},
		{ID: "raw-target", Label: "Raw target", Value: "Not persisted", Detail: "Pre-risk target is not a canonical persisted checkpoint field.", State: "PENDING"},
		{ID: "vol-target", Label: "Vol targeting", Value: "Not persisted", Detail: "Intermediate volatility scaling is not persisted; only the approved DecisionBatch is durable.", State: "PENDING"},
		{ID: "risk", Label: "Risk / Decision", Value: approved, Detail: "Approved target notional from durable PortfolioRisk DecisionBatch.", State: "OK"},
		{ID: "approved-target", Label: "Approved target", Value: approved, Detail: "Canonical target_notional_usd aggregated across strategies.", State: "OK"},
		{ID: "current-position", Label: "Current position", Value: currentLabel, Detail: "Virtual strategy position at OrderPlanner request time.", State: stageState(checkpoint.PlanningRequest != nil, "OK", "PENDING")},
		{ID: "delta", Label: "Required delta", Value: deltaLabel, Detail: "Durable planned notional delta; no quantity conversion is inferred here.", State: stageState(len(facts.plannedOrders) > 0, "CHANGED", "OK")},
		{ID: "order-plan", Label: "Order plan", Value: facts.plannedAction, Detail: "OrderPlanner durable plan at the same decision timestamp.", State: stageState(plannerReady, "OK", "PENDING")},
		{ID: "exchange", Label: "Execution state", Value: facts.exchangeState, Detail: facts.exchangeDetail, State: facts.exchangeStage},
	}
	return realPipelineAssetRow{CycleID: cycleID, Asset: facts.asset, DecisionTime: formatTradingTimestamp(checkpoint.Timestamp), RankLabel: "Not persisted", RSILabel: "Not persisted", Signal: facts.signal, RawTargetLabel: "Not persisted", VolTargetLabel: "Not persisted", ApprovedTargetLabel: approved, CurrentWeightLabel: currentLabel, RequiredDeltaLabel: deltaLabel, PlannedAction: facts.plannedAction, ExchangeState: facts.exchangeState, Stages: stages}
}

func pipelineTrace(checkpoint pgstore.PipelineCheckpoint, state pgstore.RuntimeState, facts pipelineAssetFacts, cycleID string, recon realReconciliationRow, reconErr error, mode string) realWhyTrace {
	orderIDs := make([]string, 0, len(facts.plannedOrders))
	sides := map[string]struct{}{}
	quantities := []string{}
	fillStates := []string{}
	runtimeMap := map[uint64]tradingwire.PersistedTrackedOrder{}
	for _, order := range state.Snapshot.Orders {
		runtimeMap[order.OrderID] = order
	}
	for _, order := range facts.plannedOrders {
		orderIDs = append(orderIDs, strconv.FormatUint(order.OrderID, 10))
		sides[sideLabel(order.Side)] = struct{}{}
		if tracked, ok := runtimeMap[order.OrderID]; ok {
			quantities = append(quantities, formatQuantity(tracked.Quantity)+" "+tracked.Coin)
			fillStates = append(fillStates, mapExecutionState(tracked))
		} else {
			fillStates = append(fillStates, "PENDING")
		}
	}
	side := "—"
	if len(sides) == 1 {
		for s := range sides {
			side = s
		}
	} else if len(sides) > 1 {
		side = "MULTI"
	}
	quantity := "—"
	if len(quantities) > 0 {
		quantity = strings.Join(quantities, ", ")
	}
	fillState := "No actionable order"
	if len(fillStates) > 0 {
		fillState = strings.Join(fillStates, ", ")
	}
	localQty, exchangeQty, reconStatus := formatQuantity(state.Snapshot.AccountPositions[facts.asset]), "Not available", "UNKNOWN"
	if reconErr == nil && recon.Asset != "" {
		localQty = recon.LocalQtyLabel
		exchangeQty = recon.ExchangeQtyLabel
		reconStatus = recon.Status
	}
	universe := marketDetail(checkpoint)
	freshness := "Durable checkpoint " + formatTradingTimestamp(checkpoint.Timestamp)
	if checkpoint.StrategyUpdate == nil {
		freshness = "Strategy market-update checkpoint unavailable for this decision"
	}
	transition := "Previous signal state is not persisted in this checkpoint lineage"
	intent := facts.signalDetail
	planningRevision := "Not available"
	if checkpoint.Plan != nil {
		planningRevision = strconv.FormatUint(checkpoint.Plan.StateRevision, 10)
	} else if checkpoint.PlanningRequest != nil {
		planningRevision = strconv.FormatUint(checkpoint.PlanningRequest.State.StateRevision, 10)
	}
	approved := formatUSD(facts.targetNotional)
	return realWhyTrace{
		CycleID: cycleID, Asset: facts.asset,
		Market:    realWhyMarket{Price: priceOrUnavailable(facts.referenceClose), Universe: universe, LiquidityRank: "Not persisted", Freshness: freshness},
		Strategy:  realWhyStrategy{RSI: "Not persisted", Signal: facts.signal, Transition: transition, Intent: intent},
		Portfolio: realWhyPortfolio{RawTarget: "Not persisted", VolTarget: "Not persisted", RiskChanges: []string{"Intermediate risk/volatility transformations are not persisted; DecisionBatch is the canonical approved output."}, ApprovedTarget: approved},
		Planning: realWhyPlanning{EffectiveQty: signedQuantity(facts.currentQty), PendingQty: signedQuantity(facts.pendingQty), Delta: func() string {
			if len(facts.plannedOrders) > 0 {
				return signedUSD(facts.deltaNotional)
			}
			return "No submit delta"
		}(), StateRevision: planningRevision},
		Execution:      realWhyExecution{OrderIDs: orderIDs, Side: side, Quantity: quantity, FillState: fillState, Fees: "See Execution (not duplicated in lineage read)", Slippage: "Not persisted"},
		Reconciliation: realWhyReconciliation{LocalQty: localQty, ExchangeQty: exchangeQty, Status: reconStatus},
		Audit:          realWhyAudit{CorrelationID: checkpoint.Decision.Metadata.CorrelationID, ConfigVersion: "Not persisted", Mode: mode, Exchange: "Runtime venue name not persisted", DecisionTimestamp: formatTradingTimestamp(checkpoint.Timestamp), ExecutionTimestamp: formatTradingTimestamp(state.Snapshot.LastExecutionTimestamp)},
	}
}

func aggregateSignal(batch tradingwire.StrategyIntentBatch, coin string) (string, string) {
	total := 0.0
	parts := []string{}
	for _, strategy := range batch.Strategies {
		if value, ok := strategy.Signals[coin]; ok {
			total += value
			parts = append(parts, fmt.Sprintf("strategy %d (%s): %+.3f", strategy.StrategyID, strategy.StrategyName, value))
		}
	}
	label := "FLAT"
	if total > 1e-12 {
		label = "LONG"
	} else if total < -1e-12 {
		label = "SHORT"
	}
	if len(parts) == 0 {
		return label, "No explicit signal entry for this asset in the persisted StrategyIntentBatch."
	}
	return label, strings.Join(parts, " · ")
}

func aggregateExecutionStates(states []string) (string, string, string) {
	if len(states) == 0 {
		return "NONE", "OK", "No runtime order is associated with this asset."
	}
	seen := map[string]bool{}
	for _, s := range states {
		seen[s] = true
	}
	if seen["REJECTED"] {
		return "REJECTED", "BLOCKED", "At least one planned order is durably REJECTED in ExecutionState."
	}
	if seen["PARTIAL"] {
		return "PARTIAL", "PENDING", "At least one planned order is partially filled."
	}
	if seen["PENDING_CANCEL"] || seen["NEW"] || seen["PENDING"] {
		return "PENDING", "PENDING", "Planned order execution is not terminal yet."
	}
	if seen["CANCELED"] {
		return "CANCELED", "CHANGED", "At least one planned order is durably canceled."
	}
	return "FILLED", "OK", "All associated planned orders are durably filled."
}

func marketDetail(checkpoint pgstore.PipelineCheckpoint) string {
	if checkpoint.StrategyUpdate == nil {
		return "MarketDataUpdated checkpoint unavailable at the aligned decision timestamp"
	}
	return fmt.Sprintf("%s · %s · active top %d", checkpoint.StrategyUpdate.Source, checkpoint.StrategyUpdate.Timeframe, checkpoint.StrategyUpdate.ActiveTopN)
}
func priceOrUnavailable(value float64) string {
	if value > 0 && !math.IsNaN(value) && !math.IsInf(value, 0) {
		return formatPriceUSD(value)
	}
	return "Not available"
}
func stageState(condition bool, yes, no string) string {
	if condition {
		return yes
	}
	return no
}
func signedUSD(value float64) string {
	if value > 0 {
		return "+" + formatUSD(value)
	}
	if value < 0 {
		return "-" + formatUSD(math.Abs(value))
	}
	return formatUSD(0)
}
