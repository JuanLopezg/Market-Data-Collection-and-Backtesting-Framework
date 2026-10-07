#pragma once

#include <vector>

#include "target_resolution.h"
#include "decision_batch.h"
#include "quantity_order_builder.h"
#include "price_snapshot.h"
#include "order_manager.h"
#include "portfolio_targets.h"
#include "position_state.h"
#include "portfolio.h"


struct QuantityOrderPlannerResult {
    ExecutionPlan execution_plan;
    TargetPortfolio global_target;
    OrderID next_order_id = 1;
};


// Pure target/current/pending -> cancel/submit planning boundary
//
// No account/order state is mutated and no exchange side effect is performed here.
// Quantity is resolved only from execution prices (normally open T+1), preserving the
// validated close-T decision -> open-T+1 execution semantics.
class QuantityOrderPlanner {
private:
    PortfolioAggregator portfolio_aggregator_;
    StrategyTargetResolver strategy_target_resolver_;
    QuantityTargetResolver quantity_target_resolver_;
    QuantityOrderBuilder quantity_order_builder_;

public:
    QuantityOrderPlannerResult createPlan(
        const std::vector<StrategyID>& strategyIds,
        const StrategyPositionSnapshot& strategyPositions,
        const OrderManager& orderManager,
        Timestamp executionTimestamp,
        const ExecutionReferencePrices& prices,
        const DecisionBatch& decisions,
        OrderID nextOrderId
    ) const;
};
