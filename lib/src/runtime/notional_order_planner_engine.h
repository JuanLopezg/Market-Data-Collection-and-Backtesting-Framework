#pragma once

#include <vector>

#include "decision_batch.h"
#include "execution_reference_prices.h"
#include "notional_order_planning.h"
#include "order_manager.h"
#include "portfolio_aggregator.h"
#include "strategy_position_snapshot.h"
#include "target_portfolio.h"


struct NotionalOrderPlannerResult {
    TargetPortfolio global_target;
    OrderID next_order_id = 1;
    std::vector<OrderID> cancel_order_ids;
    std::vector<PlannedNotionalOrder> submit_orders;
};


/**************************************************************************************
 * Type    : NotionalOrderPlannerEngine
 * Purpose : Pure target/current/pending -> USD notional delta planning boundary
 *
 * All valuation uses the completed close(T) supplied by ExecutionState. No live price is
 * queried. No exchange-specific quantity is produced here.
 **************************************************************************************/
class NotionalOrderPlannerEngine {
private:
    double notional_epsilon_usd_ = 1e-8;
    PortfolioAggregator portfolio_aggregator_;

public:
    explicit NotionalOrderPlannerEngine(double notionalEpsilonUsd = 1e-8);

    NotionalOrderPlannerResult createPlan(
        const std::vector<StrategyID>& strategyIds,
        const StrategyPositionSnapshot& strategyPositions,
        const OrderManager& orderManager,
        Timestamp decisionTimestamp,
        std::uint64_t stateRevision,
        const ExecutionReferencePrices& closes,
        const DecisionBatch& decisions,
        OrderID nextOrderId
    ) const;
};
