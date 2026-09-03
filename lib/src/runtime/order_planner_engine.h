#pragma once

#include <vector>

#include "account_target_resolver.h"
#include "decision_batch.h"
#include "execution_coordinator.h"
#include "execution_plan.h"
#include "execution_reference_prices.h"
#include "order_manager.h"
#include "portfolio_aggregator.h"
#include "strategy_target_resolver.h"
#include "strategy_position_snapshot.h"
#include "target_portfolio.h"


struct OrderPlannerResult {
    ExecutionPlan execution_plan;
    TargetPortfolio global_target;
    OrderID next_order_id = 1;
};


/**************************************************************************************
 * Type    : OrderPlannerEngine
 * Purpose : Pure target/current/pending -> cancel/submit planning boundary
 *
 * No account/order state is mutated and no exchange side effect is performed here.
 * Quantity is resolved only from execution prices (normally open T+1), preserving the
 * validated close-T decision -> open-T+1 execution semantics.
 **************************************************************************************/
class OrderPlannerEngine {
private:
    PortfolioAggregator portfolio_aggregator_;
    StrategyTargetResolver strategy_target_resolver_;
    AccountTargetResolver account_target_resolver_;
    ExecutionCoordinator execution_coordinator_;

public:
    OrderPlannerResult createPlan(
        const std::vector<StrategyID>& strategyIds,
        const StrategyPositionSnapshot& strategyPositions,
        const OrderManager& orderManager,
        Timestamp executionTimestamp,
        const ExecutionReferencePrices& prices,
        const DecisionBatch& decisions,
        OrderID nextOrderId
    ) const;
};
