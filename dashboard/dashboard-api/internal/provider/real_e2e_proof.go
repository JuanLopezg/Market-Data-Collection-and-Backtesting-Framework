package provider

import (
	"errors"
	"fmt"
	"math"
	"sort"
	"strconv"
	"strings"

	pgstore "control-dashboard-api/internal/integration/postgres"
	"control-dashboard-api/internal/tradingwire"
)

// realEndToEndProofData is a read-only evidence chain for the latest durable
// PortfolioRisk decision cycle. It intentionally proves only what is already
// persisted by the trading runtime. It never publishes NATS messages, requests
// exchange state, creates orders, or mutates checkpoints.
type realEndToEndProofData struct {
	Status         string                  `json:"status"`
	Completion     string                  `json:"completion"`
	CycleTimestamp string                  `json:"cycleTimestamp"`
	CorrelationID  string                  `json:"correlationId"`
	SubmitOrders   int                     `json:"submitOrders"`
	CancelOrders   int                     `json:"cancelOrders"`
	MatchedOrders  int                     `json:"matchedOrders"`
	MatchedFills   int                     `json:"matchedFills"`
	OrderIDs       []string                `json:"orderIds"`
	FillIDs        []string                `json:"fillIds"`
	Steps          []realEndToEndProofStep `json:"steps"`
	SourceNote     string                  `json:"sourceNote"`
}

type realEndToEndProofStep struct {
	ID       string `json:"id"`
	Label    string `json:"label"`
	State    string `json:"state"`
	Evidence string `json:"evidence"`
	Source   string `json:"source"`
}

func buildRealEndToEndProof(
	checkpoint pgstore.PipelineCheckpoint,
	state pgstore.RuntimeState,
	reconciliation realReconciliationData,
	reconciliationErr error,
	fills pgstore.FillWindow,
	fillsErr error,
	mode string,
) realEndToEndProofData {
	steps := make([]realEndToEndProofStep, 0, 8)
	appendStep := func(id, label, stepState, evidence, source string) {
		steps = append(steps, realEndToEndProofStep{ID: id, Label: label, State: stepState, Evidence: evidence, Source: source})
	}

	if checkpoint.StrategyUpdate != nil && checkpoint.StrategyUpdate.CompletedThrough == checkpoint.Timestamp {
		appendStep(
			"market",
			"Market Data",
			"ALIGNED",
			fmt.Sprintf("MarketDataUpdated is durably aligned to %d (%s, %s, active top %d).", checkpoint.Timestamp, checkpoint.StrategyUpdate.Source, checkpoint.StrategyUpdate.Timeframe, checkpoint.StrategyUpdate.ActiveTopN),
			"strategy_market_update_checkpoint.update_payload",
		)
	} else {
		appendStep("market", "Market Data", "PENDING", "No cycle-aligned MarketDataUpdated payload is persisted for this decision; market evidence is not inferred.", "strategy_market_update_checkpoint")
	}

	activeSignals := 0
	explicitSignals := 0
	for _, strategy := range checkpoint.Signals.Strategies {
		for _, signal := range strategy.Signals {
			explicitSignals++
			if math.Abs(signal) > 1e-12 {
				activeSignals++
			}
		}
	}
	appendStep(
		"strategy",
		"Strategy",
		"ALIGNED",
		fmt.Sprintf("StrategyIntentBatch %q is aligned to %d with %d explicit signal entries (%d active).", checkpoint.Signals.Metadata.MessageID, checkpoint.Timestamp, explicitSignals, activeSignals),
		"portfolio_risk_live_decision_checkpoint.signals_payload",
	)

	targetAssets := map[string]struct{}{}
	for _, strategy := range checkpoint.Decision.Strategies {
		for asset := range strategy.TargetNotionalUSD {
			targetAssets[asset] = struct{}{}
		}
	}
	appendStep(
		"risk",
		"Risk / Approved Target",
		"ALIGNED",
		fmt.Sprintf("DecisionBatch %q is aligned to %d and contains approved target notionals for %d asset(s).", checkpoint.Decision.Metadata.MessageID, checkpoint.Timestamp, len(targetAssets)),
		"portfolio_risk_live_decision_checkpoint.decision_payload",
	)

	planningReady := checkpoint.PlanningRequest != nil && checkpoint.Plan != nil
	submitOrders := 0
	cancelOrders := 0
	if checkpoint.Plan != nil {
		submitOrders = len(checkpoint.Plan.SubmitOrders)
		cancelOrders = len(checkpoint.Plan.CancelOrderIDs)
	}
	if planningReady {
		appendStep(
			"planning",
			"Order Planning",
			"ALIGNED",
			fmt.Sprintf("OrderPlanner state revision %d is cycle-aligned with %d submit order(s) and %d cancel request(s).", checkpoint.Plan.StateRevision, submitOrders, cancelOrders),
			"order_planner_live_notional_checkpoint.request_payload + plan_payload",
		)
	} else {
		appendStep("planning", "Order Planning", "PENDING", "The latest risk decision does not yet have both an aligned planning request and plan; no planner output is inferred.", "order_planner_live_notional_checkpoint")
	}

	runtimeByID := make(map[uint64]tradingwire.PersistedTrackedOrder, len(state.Snapshot.Orders))
	for _, order := range state.Snapshot.Orders {
		runtimeByID[order.OrderID] = order
	}
	orderIDs := make([]string, 0, submitOrders)
	matchedOrders := 0
	filledOrders := 0
	pendingOrders := 0
	rejectedOrders := 0
	canceledOrders := 0
	missingOrders := 0
	if checkpoint.Plan != nil {
		for _, planned := range checkpoint.Plan.SubmitOrders {
			orderIDs = append(orderIDs, strconv.FormatUint(planned.OrderID, 10))
			tracked, ok := runtimeByID[planned.OrderID]
			if !ok {
				missingOrders++
				continue
			}
			matchedOrders++
			switch mapExecutionState(tracked) {
			case "FILLED":
				filledOrders++
			case "REJECTED":
				rejectedOrders++
			case "CANCELED":
				canceledOrders++
			default:
				pendingOrders++
			}
		}
	}
	sort.Strings(orderIDs)

	switch {
	case !planningReady:
		appendStep("order", "Execution Order", "PENDING", "Order evidence waits for a cycle-aligned planner output.", "trading_runtime_state.snapshot.orders")
	case submitOrders == 0:
		appendStep("order", "Execution Order", "PENDING", "This is a valid no-submit cycle: OrderPlanner persisted no submit order, so there is no order to follow through execution.", "order_planner_live_notional_checkpoint.plan_payload")
	case rejectedOrders > 0:
		appendStep("order", "Execution Order", "BLOCKED", fmt.Sprintf("%d/%d planned submit order(s) are durably rejected; %d are matched in ExecutionState.", rejectedOrders, submitOrders, matchedOrders), "trading_runtime_state.snapshot.orders")
	case missingOrders > 0 || pendingOrders > 0:
		appendStep("order", "Execution Order", "PENDING", fmt.Sprintf("%d/%d planned order(s) are matched; %d pending and %d not yet present in the durable runtime snapshot.", matchedOrders, submitOrders, pendingOrders, missingOrders), "trading_runtime_state.snapshot.orders")
	case canceledOrders > 0:
		appendStep("order", "Execution Order", "ALIGNED", fmt.Sprintf("All %d planned order(s) are present; %d ended canceled and %d filled.", submitOrders, canceledOrders, filledOrders), "trading_runtime_state.snapshot.orders")
	default:
		appendStep("order", "Execution Order", "ALIGNED", fmt.Sprintf("All %d planned submit order(s) are present in durable ExecutionState and all are FILLED.", submitOrders), "trading_runtime_state.snapshot.orders")
	}

	fillsByOrder := make(map[uint64]float64)
	fillIDs := make([]string, 0, len(fills.Rows))
	matchedFillOrders := map[uint64]struct{}{}
	for _, fill := range fills.Rows {
		fillsByOrder[fill.OrderID] += fill.Quantity
		fillIDs = append(fillIDs, strconv.FormatUint(fill.FillID, 10))
		matchedFillOrders[fill.OrderID] = struct{}{}
	}
	sort.Strings(fillIDs)
	fillContradictions := 0
	if checkpoint.Plan != nil {
		for _, planned := range checkpoint.Plan.SubmitOrders {
			tracked, ok := runtimeByID[planned.OrderID]
			if !ok || mapExecutionState(tracked) != "FILLED" {
				continue
			}
			persistedQty := fillsByOrder[planned.OrderID]
			if persistedQty <= 0 || math.Abs(persistedQty-tracked.FilledQuantity) > 1e-10 {
				fillContradictions++
			}
		}
	}

	switch {
	case !planningReady || submitOrders == 0:
		appendStep("fill", "Persisted Fill", "PENDING", "No submit order exists in this latest cycle, so a fill cannot be proven for this cycle.", "trading_fills")
	case fillsErr != nil:
		appendStep("fill", "Persisted Fill", "PENDING", "Persisted fills could not be read safely: "+fillsErr.Error(), "trading_fills")
	case rejectedOrders > 0 || canceledOrders > 0:
		appendStep("fill", "Persisted Fill", "BLOCKED", fmt.Sprintf("The planned submit set contains %d rejected and %d canceled order(s); a complete order→fill chain does not exist for this cycle.", rejectedOrders, canceledOrders), "trading_runtime_state.snapshot.orders + trading_fills")
	case pendingOrders > 0 || missingOrders > 0:
		appendStep("fill", "Persisted Fill", "PENDING", fmt.Sprintf("Execution is not terminal for the complete submit set; %d fill row(s) currently match %d order ID(s).", len(fills.Rows), len(matchedFillOrders)), "trading_fills")
	case fillContradictions > 0:
		appendStep("fill", "Persisted Fill", "BLOCKED", fmt.Sprintf("%d FILLED runtime order(s) do not reconcile with durable trading_fills quantities; the dashboard refuses to call the chain proven.", fillContradictions), "trading_runtime_state.snapshot.orders + trading_fills")
	case filledOrders == submitOrders:
		appendStep("fill", "Persisted Fill", "ALIGNED", fmt.Sprintf("All %d FILLED order(s) have matching durable fill evidence (%d fill row(s)).", filledOrders, len(fills.Rows)), "trading_fills")
	default:
		appendStep("fill", "Persisted Fill", "PENDING", "Fill evidence is incomplete for the latest cycle.", "trading_fills")
	}

	if reconciliationErr != nil {
		appendStep("reconciliation", "Reconciliation", "PENDING", "Reconciliation could not be reconstructed safely: "+reconciliationErr.Error(), "trading_runtime_state + execution.exchange.snapshot.v1")
	} else {
		switch reconciliation.Status {
		case "CLEAN":
			if reconciliation.ComparisonAvailable && reconciliation.EvidenceFresh {
				appendStep("reconciliation", "Reconciliation", "ALIGNED", "Fresh retained exchange evidence matches the durable local state within runtime tolerances.", "trading_runtime_state.snapshot + execution.exchange.snapshot.v1")
			} else {
				appendStep("reconciliation", "Reconciliation", "PENDING", "Reconciliation reports CLEAN but fresh comparison evidence is not independently available to this proof.", "trading_runtime_state.snapshot + execution.exchange.snapshot.v1")
			}
		case "BLOCKED":
			appendStep("reconciliation", "Reconciliation", "BLOCKED", fmt.Sprintf("Current reconciliation is BLOCKED with %d issue(s).", len(reconciliation.Issues)), "trading_runtime_state.snapshot + execution.exchange.snapshot.v1")
		default:
			appendStep("reconciliation", "Reconciliation", "PENDING", "Current reconciliation state is "+reconciliation.Status+"; fresh closed-loop agreement is not yet proven.", "trading_runtime_state.snapshot + execution.exchange.snapshot.v1")
		}
	}

	appendStep(
		"dashboard",
		"Dashboard Projection",
		"ALIGNED",
		fmt.Sprintf("Dashboard API reconstructed this %s cycle read-only from canonical durable sources; no command/control path was invoked.", strings.ToUpper(mode)),
		"dashboard-api real provider",
	)

	status := "ALIGNED"
	completion := "FULL_CHAIN"
	for _, step := range steps {
		if step.State == "BLOCKED" {
			status = "BLOCKED"
			completion = "CONTRADICTION_OR_TERMINAL_FAILURE"
			break
		}
	}
	if status != "BLOCKED" {
		if submitOrders == 0 {
			status = "PENDING"
			completion = "NO_ACTION"
		} else {
			for _, step := range steps {
				if step.State != "ALIGNED" {
					status = "PENDING"
					completion = "INCOMPLETE_OR_IN_FLIGHT"
					break
				}
			}
		}
	}

	correlationID := strings.TrimSpace(checkpoint.Decision.Metadata.CorrelationID)
	if correlationID == "" {
		correlationID = "Not persisted"
	}
	proof := realEndToEndProofData{
		Status:         status,
		Completion:     completion,
		CycleTimestamp: formatTradingTimestamp(checkpoint.Timestamp),
		CorrelationID:  correlationID,
		SubmitOrders:   submitOrders,
		CancelOrders:   cancelOrders,
		MatchedOrders:  matchedOrders,
		MatchedFills:   len(matchedFillOrders),
		OrderIDs:       orderIDs,
		FillIDs:        fillIDs,
		Steps:          steps,
		SourceNote:     "Step 31 proves only the latest durable decision cycle. ALIGNED means the complete persisted Market→Strategy→Risk→Planner→Order→Fill→Reconciliation chain is internally consistent for a cycle with submit orders. PENDING is expected for a no-action or in-flight cycle. The dashboard never creates a trade to manufacture proof.",
	}
	if errors.Is(fillsErr, pgstore.ErrRuntimeStateNotPresent) {
		// Defensive only; FillsForOrders does not currently return this error.
		proof.SourceNote += " Runtime state was unavailable while reading fill evidence."
	}
	return proof
}
