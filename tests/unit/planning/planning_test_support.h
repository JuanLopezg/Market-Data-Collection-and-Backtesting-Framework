#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "planning/notional_order_planner.h"
#include "planning/planning_state.h"
#include "planning/quantity_order_planner.h"

namespace planning_test {

inline constexpr Timestamp kClose = 20260105;
inline constexpr Timestamp kOpen = 20260106;
inline constexpr StrategyID kStrategy = 7;
inline constexpr StrategyID kOtherStrategy = 9;

// Synthetic approved inputs shared by the two planning boundaries. They contain no
// market history, venue, clock or execution side effects.
struct Inputs {
    std::vector<StrategyID> strategy_ids{kStrategy};
    StrategyPositionSnapshot positions;
    OrderManager orders;
    ExecutionReferencePrices prices;
    DecisionBatch decisions;
    OrderID next_id = 40;
    std::uint64_t revision = 123;

    Inputs()
    {
        prices.set("BTC", 100.0);
        prices.set("ETH", 50.0);
        decisions.decision_timestamp = kClose;
        StrategyDecisionIntent intent;
        intent.strategy_id = kStrategy;
        intent.decision_timestamp = kClose;
        intent.reference_capital = 1000.0;
        decisions.strategies.push_back(std::move(intent));
    }

    void target(const Coin& coin, double usd, double weight = 0.25)
    {
        auto& intent = decisions.strategies.at(0);
        intent.decisions[coin] = RebalanceDecision::targetWeight(weight);
        intent.target_notional_usd[coin] = usd;
    }

    void flat(const Coin& coin)
    {
        auto& intent = decisions.strategies.at(0);
        intent.decisions[coin] = RebalanceDecision::flat();
        intent.target_notional_usd[coin] = 0.0;
    }

    NotionalOrderPlannerResult notional() const
    {
        return NotionalOrderPlanner().createPlan(strategy_ids, positions, orders,
            kClose, revision, prices, decisions, next_id);
    }

    QuantityOrderPlannerResult quantity(Timestamp executionDate = kOpen) const
    {
        return QuantityOrderPlanner().createPlan(strategy_ids, positions, orders,
            executionDate, prices, decisions, next_id);
    }
};

inline TrackedOrder pending(OrderSide side = OrderSide::Buy,
                            double quantity = 5.0, double filled = 2.0,
                            OrderID id = 10, StrategyID strategy = kStrategy,
                            const Coin& coin = "BTC")
{
    TrackedOrder order(ExecutionOrder(id, strategy, kClose, kOpen, coin, side, quantity));
    order.status = filled == 0.0 ? ExecutionOrderStatus::Accepted
                                 : ExecutionOrderStatus::PartiallyFilled;
    order.filled_quantity = filled;
    order.updated_at = kOpen;
    return order;
}

inline StrategyExecutionTarget quantityTarget(double quantity)
{
    StrategyExecutionTarget target;
    target.strategy_id = kStrategy;
    target.positions.set("BTC", quantity);
    return target;
}

inline VirtualPositionState held(double quantity)
{
    VirtualPositionState positions;
    positions.set("BTC", quantity);
    return positions;
}

} // namespace planning_test
