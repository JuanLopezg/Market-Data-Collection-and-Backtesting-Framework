package provider

import (
	"fmt"
	"math"
	"sort"
	"strings"
)

// Latest close decision exported by the replay: the same Strategy -> Risk -> Planner
// engines, observed stage by stage. Display only.
type simulationPipelineOrder struct {
	OrderID        string  `json:"orderId"`
	Side           string  `json:"side"`
	NotionalUSD    float64 `json:"notionalUsd"`
	DeltaUSD       float64 `json:"deltaUsd"`
	PendingUSD     float64 `json:"pendingUsd"`
	ReferenceClose float64 `json:"referenceClose"`
}

// One strategy's view of one asset; weights are fractions of that strategy's capital.
type simulationPipelineStrategy struct {
	StrategyID         uint64  `json:"strategyId"`
	Name               string  `json:"name"`
	Capital            float64 `json:"capital"`
	Share              float64 `json:"share"` // capital / total capital of all strategies
	Signal             float64 `json:"signal"`
	Detail             string  `json:"detail"` // strategy-specific indicator
	Rule               string  `json:"rule"`
	Sizer              string  `json:"sizer"`
	SizedWeight        float64 `json:"sizedWeight"`
	CappedWeight       float64 `json:"cappedWeight"`
	ApprovedWeight     float64 `json:"approvedWeight"`
	MaxAsset           float64 `json:"maxAsset"`
	MaxGross           float64 `json:"maxGross"`
	GrossAfterAssetCap float64 `json:"grossAfterAssetCap"`
	GrossScale         float64 `json:"grossScale"`
	Action             string  `json:"action"` // HOLD, FLAT, TARGET
}

// Account-level figures combine the strategies by capital share.
type simulationPipelineAsset struct {
	Asset          string                       `json:"asset"`
	Close          float64                      `json:"close"`
	Liquidity      *float64                     `json:"liquidity"`
	LiquidityRank  int                          `json:"liquidityRank"` // 0 = not rankable
	InUniverse     bool                         `json:"inUniverse"`
	Strategies     []simulationPipelineStrategy `json:"strategies"`
	Signal         float64                      `json:"signal"`         // aggregated, [-1, +1]
	SizedWeight    float64                      `json:"sizedWeight"`    // fraction of total capital
	CappedWeight   float64                      `json:"cappedWeight"`   // after the per-asset caps
	ApprovedWeight float64                      `json:"approvedWeight"` // after the gross scales
	Action         string                       `json:"action"`         // HOLD, FLAT, TARGET
	TargetUSD      *float64                     `json:"targetUsd"`      // nil: HOLD, no new target
	CurrentQty     float64                      `json:"currentQty"`
	CurrentUSD     float64                      `json:"currentUsd"`
	Orders         []simulationPipelineOrder    `json:"orders"`
}

type simulationPipeline struct {
	Date              string                    `json:"date"`
	DecisionTimestamp uint64                    `json:"decisionTimestamp"`
	LiquidityLength   int                       `json:"liquidityLength"`
	LiquidityTopN     int                       `json:"liquidityTopN"`
	TotalCapital      float64                   `json:"totalCapital"`
	ApprovedGross     float64                   `json:"approvedGross"`
	StrategyCount     int                       `json:"strategyCount"`
	ObservedAssets    int                       `json:"observedAssets"`
	UniverseSize      int                       `json:"universeSize"`
	ActiveSignals     int                       `json:"activeSignals"`
	CancelOrders      int                       `json:"cancelOrders"`
	Assets            []simulationPipelineAsset `json:"assets"`
}

func (p *Simulation) pipeline(s simulationSnapshot) realPipelineData {
	if s.Pipeline != nil {
		return simulationDecisionPipeline(s, *s.Pipeline)
	}
	return simulationObservedPipeline(s)
}

// ENTER / RESIZE / EXIT / HOLD from the rebalance action and the position held at decision time.
func pipelineDecision(action string, currentQty float64) string {
	switch action {
	case "FLAT":
		return "EXIT"
	case "TARGET":
		if currentQty == 0 {
			return "ENTER"
		}
		return "RESIZE"
	}
	return "HOLD"
}

func pipelineSignal(value float64) string {
	if value > 1e-12 {
		return "LONG"
	}
	if value < -1e-12 {
		return "SHORT"
	}
	return "FLAT"
}

// Signal in [-1, +1] with two decimals; 0 is flat.
func formatSignal(value float64) string {
	if math.Abs(value) < 0.005 {
		return "0.00"
	}
	return fmt.Sprintf("%+.2f", value)
}

// "15.3B" / "820.5M" / "12.4K" for large volumes.
func compactNumber(value *float64) string {
	if value == nil || math.IsNaN(*value) || math.IsInf(*value, 0) {
		return "—"
	}
	for _, unit := range []struct {
		size   float64
		suffix string
	}{{1e9, "B"}, {1e6, "M"}, {1e3, "K"}} {
		if math.Abs(*value) >= unit.size {
			return fmt.Sprintf("%.1f%s", *value/unit.size, unit.suffix)
		}
	}
	return fmt.Sprintf("%.0f", *value)
}

// Per-strategy breakdown rows and the risk transformations of each strategy.
func simulationStrategyBreakdown(asset simulationPipelineAsset) ([]realPipelineStrategy, []string) {
	lines := make([]realPipelineStrategy, 0, len(asset.Strategies))
	changes := []string{}
	for _, strategy := range asset.Strategies {
		detail := strategy.Detail
		if detail == "" {
			detail = "—"
		}
		lines = append(lines, realPipelineStrategy{Name: strategy.Name, CapitalLabel: formatDollars(strategy.Capital),
			ShareLabel: formatPercent(strategy.Share * 100), Signal: strategy.Signal, SignalLabel: formatSignal(strategy.Signal),
			Detail: detail, Rule: strategy.Rule, SizedLabel: formatPercent(strategy.SizedWeight * 100),
			ApprovedLabel: formatPercent(strategy.ApprovedWeight * 100), Decision: pipelineDecision(strategy.Action, asset.CurrentQty)})
		changes = append(changes,
			fmt.Sprintf("%s · %s sizing %s → asset cap %s: %s", strategy.Name, strategy.Sizer, formatPercent(strategy.SizedWeight*100), formatPercent(strategy.MaxAsset*100), formatPercent(strategy.CappedWeight*100)),
			fmt.Sprintf("%s · gross %s ≤ %s, scale %.3fx: %s", strategy.Name, formatPercent(strategy.GrossAfterAssetCap*100), formatPercent(strategy.MaxGross*100), strategy.GrossScale, formatPercent(strategy.ApprovedWeight*100)))
	}
	return lines, changes
}

func simulationDecisionPipeline(s simulationSnapshot, view simulationPipeline) realPipelineData {
	ordersByID := map[string]simulationOrder{}
	for _, order := range s.Orders {
		ordersByID[order.OrderID] = order
	}
	accountQty := map[string]float64{}
	for _, position := range s.Account.Positions {
		accountQty[position.Asset] = position.Quantity
	}

	rows := make([]realPipelineAssetRow, 0, len(view.Assets))
	traces := map[string]realWhyTrace{}
	plannedOrders := 0
	for _, asset := range view.Assets {
		// Stable per asset so an open row or inspector survives the next replayed day.
		id := "replay-" + asset.Asset
		decision := pipelineDecision(asset.Action, asset.CurrentQty)
		signal := formatSignal(asset.Signal)
		strategies, riskChanges := simulationStrategyBreakdown(asset)

		// Planned orders execute at the next open; the venue order carries the same id.
		actions, ids, states, quantities := []string{}, []string{}, []string{}, []string{}
		delta, pending := 0.0, 0.0
		side, executedAt := "—", "—"
		if len(asset.Orders) > 0 {
			executedAt = "Not routed yet"
		}
		for _, order := range asset.Orders {
			plannedOrders++
			actions = append(actions, order.Side+" "+formatDollars(order.NotionalUSD))
			ids = append(ids, order.OrderID)
			delta += order.DeltaUSD
			pending += order.PendingUSD
			side = order.Side
			if routed, ok := ordersByID[order.OrderID]; ok {
				states = append(states, executionOrderState(routed.Status))
				quantities = append(quantities, fmt.Sprintf("%s / %s %s filled", formatQuantity(routed.FilledQuantity), formatQuantity(routed.Quantity), routed.Asset))
				// Replay business time is YYYYMMDD plus a phase digit (1 = open).
				executedAt = formatTradingTimestamp(routed.ActiveFrom/10) + " open"
			} else {
				states = append(states, "PENDING")
			}
		}
		exchange, exchangeStage, exchangeDetail := "NONE", "OK", "No order needed for this asset."
		if len(states) > 0 {
			exchange, exchangeStage, exchangeDetail = aggregateExecutionStates(states)
		}
		plannedAction, deltaLabel := "—", "—"
		if len(actions) > 0 {
			plannedAction, deltaLabel = strings.Join(actions, " + "), signedDollars(delta)
		}

		rankLabel, liquidityRank := "—", "Not rankable (no SMA history)"
		if asset.LiquidityRank > 0 {
			rankLabel = fmt.Sprintf("#%d", asset.LiquidityRank)
			liquidityRank = fmt.Sprintf("#%d of %d by SMA(volume, %d)", asset.LiquidityRank, view.ObservedAssets, view.LiquidityLength)
		}
		universe := fmt.Sprintf("Outside the liquidity top %d", view.LiquidityTopN)
		if asset.InUniverse {
			universe = fmt.Sprintf("Inside the liquidity top %d", view.LiquidityTopN)
		}
		target, approved := "—", formatPercent(asset.ApprovedWeight*100)
		if asset.TargetUSD != nil {
			target = formatDollars(*asset.TargetUSD)
		}
		current := "—"
		if asset.CurrentQty != 0 {
			current = formatDollars(asset.CurrentUSD)
		}
		indicators, intents := []string{}, []string{}
		for _, strategy := range strategies {
			indicators = append(indicators, strategy.Name+": "+strategy.Detail)
			intents = append(intents, fmt.Sprintf("%s %s × %s capital", strategy.Name, strategy.SignalLabel, strategy.ShareLabel))
		}
		transition := "No change: the rebalance policy keeps the current position."
		switch decision {
		case "ENTER":
			transition = "Entry: aggregated signal " + signal + " with no position held."
		case "EXIT":
			transition = "Exit: the strategies no longer signal this asset."
		case "RESIZE":
			transition = "Resize to the approved weight."
		}
		inUniverse := asset.InUniverse

		stages := []realPipelineStage{
			{ID: "market", Label: "Market", Value: formatPrice(asset.Close), Detail: "Close " + view.Date + " · SMA volume " + compactNumber(asset.Liquidity), State: "OK"},
			{ID: "universe", Label: "Universe", Value: rankLabel, Detail: universe, State: stageState(inUniverse, "OK", "PENDING")},
			{ID: "signal", Label: "Aggregated signal", Value: signal, Detail: "Sum of strategy signals weighted by capital share", State: "OK"},
			{ID: "sizing", Label: "Sizing", Value: formatPercent(asset.SizedWeight * 100), Detail: "Sizer weights combined by capital share", State: "OK"},
			{ID: "risk", Label: "Risk caps", Value: approved, Detail: "Per-strategy asset cap and gross scale, combined by capital share", State: stageState(asset.ApprovedWeight != asset.SizedWeight, "CHANGED", "OK")},
			{ID: "decision", Label: "Decision", Value: decision, Detail: transition, State: stageState(decision == "HOLD", "OK", "CHANGED")},
			{ID: "approved-target", Label: "Target", Value: target, Detail: "Approved notional at capital " + formatDollars(view.TotalCapital), State: "OK"},
			{ID: "current-position", Label: "Current", Value: current, Detail: signedQuantity(asset.CurrentQty) + " " + asset.Asset + " at decision time", State: "OK"},
			{ID: "delta", Label: "Delta", Value: deltaLabel, Detail: "Target − current − pending orders", State: stageState(len(asset.Orders) > 0, "CHANGED", "OK")},
			{ID: "order-plan", Label: "Order", Value: plannedAction, Detail: "Planned at close, executed at the next open", State: "OK"},
			{ID: "exchange", Label: "Execution", Value: exchange, Detail: exchangeDetail, State: exchangeStage},
		}
		signalValue := asset.Signal
		rows = append(rows, realPipelineAssetRow{CycleID: id, Asset: asset.Asset, DecisionTime: view.Date,
			Rank: asset.LiquidityRank, RankLabel: rankLabel, RSILabel: "—", Signal: pipelineSignal(asset.Signal),
			RawTargetPct: asset.SizedWeight * 100, RawTargetLabel: formatPercent(asset.SizedWeight * 100),
			VolTargetLabel: "Not applicable", ApprovedTargetPct: asset.ApprovedWeight * 100, ApprovedTargetLabel: approved,
			CurrentWeightLabel: current, RequiredDeltaLabel: deltaLabel, PlannedAction: plannedAction, ExchangeState: exchange,
			Stages: stages, DecisionLabel: decision, TargetLabel: target, InUniverse: &inUniverse,
			SignalValue: &signalValue, Strategies: strategies})

		pendingLabel := "0"
		if pending != 0 {
			pendingLabel = signedDollars(pending)
		}
		quantity := "—"
		if len(quantities) > 0 {
			quantity = strings.Join(quantities, ", ")
		}
		fillState := "No order"
		if len(states) > 0 {
			fillState = strings.Join(states, ", ")
		}
		traces[id] = realWhyTrace{CycleID: id, Asset: asset.Asset,
			Market:         realWhyMarket{Price: formatPrice(asset.Close), Universe: universe, LiquidityRank: liquidityRank, Freshness: "Daily close " + view.Date},
			Strategy:       realWhyStrategy{RSI: strings.Join(indicators, " · "), Signal: signal + " aggregated", Transition: transition, Intent: strings.Join(intents, " + ")},
			Portfolio:      realWhyPortfolio{RawTarget: formatPercent(asset.SizedWeight * 100), VolTarget: "Not applicable (no volatility sizer)", RiskChanges: append(riskChanges, "Decision "+decision+", target "+target), ApprovedTarget: approved},
			Planning:       realWhyPlanning{EffectiveQty: signedQuantity(asset.CurrentQty), PendingQty: pendingLabel, Delta: deltaLabel, StateRevision: "decision " + view.Date},
			Execution:      realWhyExecution{OrderIDs: ids, Side: side, Quantity: quantity, FillState: fillState, Fees: "See Execution", Slippage: "None (fills at the open price)"},
			Reconciliation: realWhyReconciliation{LocalQty: signedQuantity(asset.CurrentQty), ExchangeQty: signedQuantity(accountQty[asset.Asset]) + " (now)", Status: s.Reconciliation.State},
			Audit: realWhyAudit{CorrelationID: id, ConfigVersion: fmt.Sprintf("%d strategies", view.StrategyCount),
				Mode: "REPLAY", Exchange: s.VenueID, DecisionTimestamp: view.Date, ExecutionTimestamp: executedAt}}
	}

	return realPipelineData{
		CycleLabel:      "Decision " + view.Date,
		LatestDecision:  view.Date,
		UniverseSize:    view.UniverseSize,
		ActiveSignals:   view.ActiveSignals,
		ActionablePlans: plannedOrders + view.CancelOrders,
		Rows:            rows,
		Traces:          traces,
		SourceMode:      "MOCK",
		SourceUpdatedAt: fmt.Sprintf("%d", s.BusinessTimestamp),
		EvidenceState:   "OBSERVED",
		EmptyReason:     "No asset in the universe, with a signal, a position or an order.",
		Summary: []realOverviewMetric{
			{Label: "Decision", Value: view.Date},
			{Label: "Strategies", Value: fmt.Sprintf("%d", view.StrategyCount)},
			{Label: "Universe", Value: fmt.Sprintf("%d / %d", view.UniverseSize, view.ObservedAssets)},
			{Label: "Active Signals", Value: fmt.Sprintf("%d", view.ActiveSignals)},
			{Label: "Orders", Value: fmt.Sprintf("%d", plannedOrders+view.CancelOrders)},
			{Label: "Approved Gross", Value: formatPercent(view.ApprovedGross * 100)},
		},
	}
}

// Older replay states carry no decision lineage: show the observed market, positions and
// orders only, without inferring signals or targets.
func simulationObservedPipeline(s simulationSnapshot) realPipelineData {
	assets := map[string]bool{}
	prices := map[string]float64{}
	quantities := map[string]float64{}
	orders := map[string][]simulationOrder{}
	for _, bar := range s.Market {
		assets[bar.Asset] = true
		prices[bar.Asset] = bar.Close
	}
	for _, position := range s.Account.Positions {
		assets[position.Asset] = true
		quantities[position.Asset] = position.Quantity
	}
	for _, order := range s.Orders {
		assets[order.Asset] = true
		orders[order.Asset] = append(orders[order.Asset], order)
	}
	names := make([]string, 0, len(assets))
	for asset := range assets {
		if asset != "" {
			names = append(names, asset)
		}
	}
	sort.Strings(names)
	rows := make([]realPipelineAssetRow, 0, len(names))
	traces := map[string]realWhyTrace{}
	for _, asset := range names {
		id := fmt.Sprintf("replay-%d-%s", s.Generation, asset)
		observed := orders[asset]
		states := []string{}
		ids := []string{}
		for _, order := range observed {
			states = append(states, executionOrderState(order.Status))
			ids = append(ids, order.OrderID)
		}
		state, stage, detail := "NONE", "PENDING", "No retained order for this asset; this is not proof of a completed no-order strategy cycle."
		if len(states) > 0 {
			state, stage, detail = aggregateExecutionStates(states)
		}
		action := fmt.Sprintf("%d retained orders", len(observed))
		quantity := signedQuantity(quantities[asset])
		unavailable := "Not exposed in replay snapshot"
		stages := []realPipelineStage{
			{ID: "market", Label: "Observed market", Value: priceOrUnavailable(prices[asset]), Detail: s.HistoricalDate + " · completed replay observation; not a strategy universe/rank", State: "OK"},
			{ID: "signal", Label: "Strategy signal", Value: "UNKNOWN", Detail: unavailable, State: "PENDING"},
			{ID: "risk", Label: "Risk / sizing", Value: unavailable, Detail: "Raw sizing, constraints and approved decision lineage are not present in this snapshot.", State: "PENDING"},
			{ID: "current-position", Label: "Observed position", Value: quantity, Detail: "Current canonical MOCK account position, not the decision-time planner input.", State: "OK"},
			{ID: "order-plan", Label: "Order plan", Value: unavailable, Detail: "Retained execution orders are observations, not a same-cycle planner checkpoint.", State: "PENDING"},
			{ID: "exchange", Label: "Retained execution", Value: state, Detail: detail, State: stage},
		}
		rows = append(rows, realPipelineAssetRow{CycleID: id, Asset: asset, DecisionTime: s.HistoricalDate,
			RankLabel: unavailable, RSILabel: unavailable, Signal: "UNKNOWN", RawTargetLabel: unavailable, VolTargetLabel: unavailable,
			ApprovedTargetLabel: unavailable, CurrentWeightLabel: quantity, RequiredDeltaLabel: unavailable,
			PlannedAction: action, ExchangeState: state, Stages: stages})
		traces[id] = realWhyTrace{CycleID: id, Asset: asset,
			Market:         realWhyMarket{Price: priceOrUnavailable(prices[asset]), Universe: "Observed assets, not reconstructed selection", LiquidityRank: unavailable, Freshness: s.HistoricalDate},
			Strategy:       realWhyStrategy{RSI: unavailable, Signal: "UNKNOWN", Transition: unavailable, Intent: unavailable},
			Portfolio:      realWhyPortfolio{RawTarget: unavailable, VolTarget: unavailable, ApprovedTarget: unavailable, RiskChanges: []string{unavailable}},
			Planning:       realWhyPlanning{EffectiveQty: unavailable, PendingQty: unavailable, Delta: unavailable, StateRevision: unavailable},
			Execution:      realWhyExecution{OrderIDs: ids, Side: unavailable, Quantity: quantity, FillState: state, Fees: "See Execution", Slippage: unavailable},
			Reconciliation: realWhyReconciliation{LocalQty: quantity, ExchangeQty: unavailable, Status: s.Reconciliation.State},
			Audit:          realWhyAudit{Mode: "MOCK REPLAY", Exchange: s.VenueID, DecisionTimestamp: unavailable, ExecutionTimestamp: fmt.Sprintf("%d", s.BusinessTimestamp)}}
	}
	return realPipelineData{CycleLabel: s.HistoricalDate + " · " + s.Phase, LatestDecision: "Not exposed", UniverseSize: len(names),
		Rows: rows, Traces: traces, SourceMode: "MOCK", SourceUpdatedAt: fmt.Sprintf("%d", s.BusinessTimestamp), EvidenceState: "PARTIAL",
		EmptyReason: "The current replay snapshot contains no market, position or retained-order observations.",
		SourceNote:  "Replay observations only: no strategy signals, approved risk instructions or same-cycle planner payloads are exposed. Retained orders/fills can span earlier cycles and are not end-to-end proof."}
}

func (p *Simulation) risk(s simulationSnapshot) realRiskData {
	rows := []realRiskAsset{}
	for _, position := range s.Account.Positions {
		weight := "Not valued"
		if s.Account.Equity > 0 {
			weight = formatPercent(position.Quantity * position.MarkPrice / s.Account.Equity * 100)
		}
		rows = append(rows, realRiskAsset{Asset: position.Asset, Signal: "UNKNOWN", ApprovedWeightLabel: "Not exposed",
			TargetNotionalLabel: "Not exposed", StrategyLabel: "Not exposed", CurrentWeightLabel: weight, LimitLabel: "Not exposed"})
	}
	sort.Slice(rows, func(i, j int) bool { return rows[i].Asset < rows[j].Asset })
	data := realRiskData{RiskState: "REPLAY ACCOUNT OBSERVED", SourceMode: "MOCK", EvidenceKind: "REPLAY_SNAPSHOT",
		SourceUpdatedAt: fmt.Sprintf("%d", s.BusinessTimestamp), AccountCashLabel: formatUSD2(s.Account.CashTotal),
		ReferenceCapitalLabel: formatUSD2(s.Account.Equity), ApprovedGrossTargetLabel: "Not exposed", ApprovedNetTargetLabel: "Not exposed",
		ApprovedTargetNotionalLabel: "Not exposed", Assets: rows, Policies: []riskPolicy{}, CurrentValuationAvailable: true,
		DecisionTimestamp: "Not exposed", DecisionDetail: "Current canonical MOCK account observation. Strategy/risk decision inputs, limits and binding-rule reports are not present; no zero-risk or armed kill-switch verdict is inferred.",
		SourceNote:         "Canonical replay account and marked positions. Reconciliation: " + s.Reconciliation.State + ". This page does not reconstruct historical sizing or approve private trading.",
		MissingDiagnostics: []string{"Strategy intents and approved risk decision for the same cycle", "Active strategy allocation/sizing/constraint configuration", "Raw and volatility-adjusted sizing, binding constraints and breach report", "Kill-switch configuration and live-capital readiness"}}
	simulationRiskView(&data, s)
	return data
}

// Risk page view from the replay: marked account, daily equity/gross history and the
// latest decision's per-strategy risk evaluation.
func simulationRiskView(data *realRiskData, s simulationSnapshot) {
	points := make([]riskEquityPoint, 0, len(s.EquityHistory))
	for _, point := range s.EquityHistory {
		points = append(points, riskEquityPoint{label: point.Label, equity: point.Equity, gross: point.Gross})
	}
	history, maxDrawdown := riskHistory(points)

	approved := map[string]float64{}
	assetCap := 0.0
	strategies := map[uint64]simulationPipelineStrategy{}
	capReduced := map[uint64]bool{}
	order := []uint64{}
	if s.Pipeline != nil {
		for _, asset := range s.Pipeline.Assets {
			approved[asset.Asset] = asset.ApprovedWeight
			for _, strategy := range asset.Strategies {
				if _, seen := strategies[strategy.StrategyID]; !seen {
					strategies[strategy.StrategyID] = strategy
					order = append(order, strategy.StrategyID)
				}
				if math.Abs(strategy.CappedWeight) < math.Abs(strategy.SizedWeight)-1e-12 {
					capReduced[strategy.StrategyID] = true
				}
				// Account-level per-asset cap: each strategy's cap on its share of capital.
				assetCap = math.Max(assetCap, strategy.MaxAsset*strategy.Share)
			}
		}
	}

	positions := make([]riskPositionValue, 0, len(s.Account.Positions))
	for _, position := range s.Account.Positions {
		row := riskPositionValue{asset: position.Asset, value: position.Quantity * position.MarkPrice}
		if weight, ok := approved[position.Asset]; ok {
			row.approved = &weight
		}
		positions = append(positions, row)
	}
	data.Summary, data.Exposures = riskAccountView(s.Account.Equity, positions, assetCap, history, maxDrawdown)
	data.History = history

	for _, id := range order {
		strategy := strategies[id]
		data.StrategyLimits = append(data.StrategyLimits, riskStrategyLimit{Name: strategy.Name, Capital: formatDollars(strategy.Capital),
			Sizer: strategy.Sizer, MaxAsset: formatPercent(strategy.MaxAsset * 100), MaxGross: formatPercent(strategy.MaxGross * 100),
			SizedGross: formatPercent(strategy.GrossAfterAssetCap * 100), Scale: fmt.Sprintf("%.3fx", strategy.GrossScale),
			ApprovedGross: formatPercent(strategy.GrossAfterAssetCap * strategy.GrossScale * 100),
			Binding:       riskBinding(strategy.GrossScale, capReduced[id])})
	}
}
