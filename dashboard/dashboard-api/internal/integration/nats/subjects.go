package nats

// Canonical trading-runtime subjects verified against
// lib/src/transport/transport_subjects.h in the uploaded project snapshot.
// Keep this list synchronized with the C++ transport contract; Step 14 ships a
// source-audit script that fails if those contract strings drift.
const (
	SubjectMarketDataRelease            = "market.release.v1"
	SubjectMarketDataUpdated            = "market.data.updated.v1"
	SubjectMarketSliceClosed            = "market.slice.closed.v1"
	SubjectMarketSliceSnapshot          = "market.slice.snapshot.v1"
	SubjectStrategyIntents              = "strategy.intents.v1"
	SubjectDecisionBatch                = "decision.batch.v1"
	SubjectExecutionPrices              = "execution.prices.v1"
	SubjectSubmitOrder                  = "execution.command.submit.v1"
	SubjectCancelOrder                  = "execution.command.cancel.v1"
	SubjectOrderUpdate                  = "execution.event.order_update.v1"
	SubjectFill                         = "execution.event.fill.v1"
	SubjectExecutionEventFilter         = "execution.event.>"
	SubjectAccountSnapshot              = "execution.account.snapshot.v1"
	SubjectExecutionCycleComplete       = "execution.cycle.complete.v1"
	SubjectExchangeSnapshot             = "execution.exchange.snapshot.v1"
	SubjectExchangeSnapshotRequest      = "execution.exchange.snapshot.request.v1"
	SubjectOrderPlanningRequest         = "execution.plan.request.v1"
	SubjectOrderPlan                    = "execution.plan.v1"
	SubjectNotionalOrderPlanningRequest = "execution.plan.notional.request.v1"
	SubjectNotionalOrderPlan            = "execution.plan.notional.v1"
)

var AuditedRuntimeSubjects = []string{
	SubjectMarketDataRelease,
	SubjectMarketDataUpdated,
	SubjectMarketSliceClosed,
	SubjectMarketSliceSnapshot,
	SubjectStrategyIntents,
	SubjectDecisionBatch,
	SubjectExecutionPrices,
	SubjectSubmitOrder,
	SubjectCancelOrder,
	SubjectOrderUpdate,
	SubjectFill,
	SubjectAccountSnapshot,
	SubjectExecutionCycleComplete,
	SubjectExchangeSnapshot,
	SubjectOrderPlanningRequest,
	SubjectOrderPlan,
	SubjectNotionalOrderPlanningRequest,
	SubjectNotionalOrderPlan,
}
