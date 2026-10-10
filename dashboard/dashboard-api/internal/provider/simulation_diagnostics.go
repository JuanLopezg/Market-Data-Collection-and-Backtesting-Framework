package provider

import (
	"fmt"
	"sort"
)

// Replay snapshots expose market/execution/account observations, not service decision checkpoints.
// Keep these read models separate from exact Strategy -> Risk -> Planner lineage.
func (p *Simulation) pipeline(s simulationSnapshot) realPipelineData {
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
	return realRiskData{RiskState: "REPLAY ACCOUNT OBSERVED", SourceMode: "MOCK", EvidenceKind: "REPLAY_SNAPSHOT",
		SourceUpdatedAt: fmt.Sprintf("%d", s.BusinessTimestamp), AccountCashLabel: formatUSD2(s.Account.CashTotal),
		ReferenceCapitalLabel: formatUSD2(s.Account.Equity), ApprovedGrossTargetLabel: "Not exposed", ApprovedNetTargetLabel: "Not exposed",
		ApprovedTargetNotionalLabel: "Not exposed", Assets: rows, Policies: []riskPolicy{}, CurrentValuationAvailable: true,
		DecisionTimestamp: "Not exposed", DecisionDetail: "Current canonical MOCK account observation. Strategy/risk decision inputs, limits and binding-rule reports are not present; no zero-risk or armed kill-switch verdict is inferred.",
		SourceNote:         "Canonical replay account and marked positions. Reconciliation: " + s.Reconciliation.State + ". This page does not reconstruct historical sizing or approve private trading.",
		MissingDiagnostics: []string{"Strategy intents and approved risk decision for the same cycle", "Active strategy allocation/sizing/constraint configuration", "Raw and volatility-adjusted sizing, binding constraints and breach report", "Kill-switch configuration and live-capital readiness"}}
}
