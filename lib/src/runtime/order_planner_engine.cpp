#include "order_planner_engine.h"

#include <stdexcept>
#include <unordered_map>
#include <unordered_set>


OrderPlannerResult OrderPlannerEngine::createPlan(
    const std::vector<StrategyID>& strategyIds,
    const StrategyPositionSnapshot& strategyPositions,
    const OrderManager& orderManager,
    Timestamp executionTimestamp,
    const ExecutionReferencePrices& prices,
    const DecisionBatch& decisions,
    OrderID nextOrderId
) const
{
    if (nextOrderId == 0)
        throw std::invalid_argument("Order planner next order id must be non-zero");

    if (decisions.strategies.empty()) {
        OrderPlannerResult empty;
        empty.next_order_id = nextOrderId;
        return empty;
    }

    std::unordered_set<StrategyID> configuredIds;
    configuredIds.reserve(strategyIds.size());
    for (const StrategyID strategyId : strategyIds) {
        if (!configuredIds.insert(strategyId).second)
            throw std::invalid_argument("Order planner contains duplicate configured strategy id");
    }

    std::unordered_map<StrategyID, const StrategyDecisionIntent*> intents;
    intents.reserve(decisions.strategies.size());
    for (const StrategyDecisionIntent& intent : decisions.strategies) {
        if (!configuredIds.contains(intent.strategy_id))
            throw std::invalid_argument("Decision batch contains unknown strategy id");
        if (intent.decision_timestamp != decisions.decision_timestamp)
            throw std::invalid_argument("Decision intent timestamp does not match batch");
        if (!intents.emplace(intent.strategy_id, &intent).second)
            throw std::invalid_argument("Decision batch contains duplicate strategy id");
    }

    for (const auto& [strategyId, positions] : strategyPositions) {
        (void)positions;
        if (!configuredIds.contains(strategyId))
            throw std::invalid_argument("Planning state contains unknown strategy position id");
    }

    std::vector<TargetPortfolio> monetaryTargets;
    std::vector<StrategyExecutionTarget> quantityTargets;
    std::vector<VirtualPositionState> currentStrategyPositions;

    monetaryTargets.reserve(strategyIds.size());
    quantityTargets.reserve(strategyIds.size());
    currentStrategyPositions.reserve(strategyIds.size());

    for (const StrategyID strategyId : strategyIds) {
        const auto positionIt = strategyPositions.find(strategyId);
        const VirtualPositionState emptyPositions;
        const VirtualPositionState& currentPositions =
            positionIt == strategyPositions.end() ? emptyPositions : positionIt->second;

        TargetPortfolio monetaryTarget;
        const auto intentIt = intents.find(strategyId);
        if (intentIt != intents.end()) {
            monetaryTarget = strategy_target_resolver_.resolve(
                *intentIt->second,
                currentPositions,
                prices
            );
        }
        else {
            // No new decision means preserve the already-filled strategy quantities.
            for (const auto& [coin, quantity] : currentPositions.values()) {
                if (!prices.contains(coin))
                    throw std::runtime_error("Missing execution price for held strategy asset");
                monetaryTarget.set(coin, quantity * prices.get(coin));
            }
        }

        monetaryTargets.push_back(monetaryTarget);

        TargetPositionState quantityTarget =
            account_target_resolver_.resolve(monetaryTarget, prices);

        // HOLD means preserve the already-filled quantity exactly. Converting a held
        // quantity through monetary exposure (q * price) and back (exposure / price)
        // can introduce a tiny floating-point residue. That residue is not an economic
        // rebalance and must not become a microscopic buy/sell order.
        for (const auto& [coin, quantity] : currentPositions.values()) {
            bool preserveFilledQuantity = true;

            if (intentIt != intents.end()) {
                const auto decisionIt = intentIt->second->decisions.find(coin);
                if (decisionIt != intentIt->second->decisions.end() &&
                    decisionIt->second.action != RebalanceAction::Hold) {
                    preserveFilledQuantity = false;
                }
            }

            if (preserveFilledQuantity)
                quantityTarget.set(coin, quantity);
        }

        quantityTargets.push_back({
            strategyId,
            std::move(quantityTarget)
        });
        currentStrategyPositions.push_back(currentPositions);
    }

    OrderPlannerResult result;
    result.global_target = portfolio_aggregator_.aggregate(monetaryTargets);
    result.next_order_id = nextOrderId;
    result.execution_plan = execution_coordinator_.createMarketPlan(
        quantityTargets,
        currentStrategyPositions,
        orderManager,
        executionTimestamp,
        executionTimestamp,
        result.next_order_id
    );
    return result;
}
