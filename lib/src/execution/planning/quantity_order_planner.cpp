#include "quantity_order_planner.h"

#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>


QuantityOrderPlannerResult QuantityOrderPlanner::createPlan(
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
        QuantityOrderPlannerResult empty;
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

    // The target and position vectors below share strategyIds order; the builder
    // uses that alignment to compare each strategy with its own filled quantities.
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
        ExecutionReferencePrices sizingPrices = prices;
        if (intentIt != intents.end()) {
            for (const auto& [coin, decision] : intentIt->second->decisions) {
                if (decision.entry_stop_price == 0.0)
                    continue;
                if (decision.action != RebalanceAction::TargetWeight ||
                    decision.target_weight == 0.0 || currentPositions.get(coin) != 0.0 ||
                    !std::isfinite(decision.entry_stop_price) || decision.entry_stop_price <= 0.0)
                    throw std::invalid_argument("Stop intent must open a flat target with a valid trigger");
                // The execution open is already known here. A short gap increases
                // units, so submit its actual executable quantity rather than an
                // entry-price cap that would violate fill quantity validation.
                sizingPrices.set(coin, decision.target_weight < 0.0 && prices.contains(coin)
                    ? std::min(decision.entry_stop_price, prices.get(coin))
                    : decision.entry_stop_price);
            }
        }
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
            quantity_target_resolver_.resolve(monetaryTarget, sizingPrices);

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

    QuantityOrderPlannerResult result;
    result.global_target = portfolio_aggregator_.aggregate(monetaryTargets);
    result.next_order_id = nextOrderId;
    // Protective covers are conditional exits, not pending directional exposure.
    // Remove them from the builder view, then cancel a bracket before any resize.
    OrderManager directionalOrders;
    std::vector<TrackedOrder> directionalState;
    const bool hasProtectiveOrders = std::any_of(orderManager.orders().begin(), orderManager.orders().end(),
        [](const auto& item) { return item.second.isOpen() && item.second.request.parent_order_id != 0; });
    if (hasProtectiveOrders) {
        for (const auto& [id, order] : orderManager.orders()) {
            (void)id;
            if (order.request.parent_order_id == 0)
                directionalState.push_back(order);
        }
        directionalOrders.restore(directionalState, {});
    }
    result.execution_plan = quantity_order_builder_.createMarketPlan(
        quantityTargets,
        currentStrategyPositions,
        hasProtectiveOrders ? directionalOrders : orderManager,
        executionTimestamp,
        executionTimestamp,
        result.next_order_id
    );
    for (auto& order : result.execution_plan.orders_to_submit) {
        for (const auto& [id, tracked] : orderManager.orders()) {
            if (tracked.request.parent_order_id != 0 && tracked.isOpen() && !tracked.cancel_requested &&
                tracked.request.strategy_id == order.strategy_id && tracked.request.coin == order.coin)
                result.execution_plan.order_ids_to_cancel.push_back(id);
        }
        const auto intent = intents.find(order.strategy_id);
        if (intent == intents.end())
            continue;
        const auto decision = intent->second->decisions.find(order.coin);
        if (decision != intent->second->decisions.end() && decision->second.entry_stop_price != 0.0) {
            if (executionTimestamp <= intent->second->decision_timestamp)
                throw std::invalid_argument("Stop entry must activate after its decision close");
            order.entry_stop_price = decision->second.entry_stop_price;
            order.protective_stop_price = decision->second.protective_stop_price;
            order.created_at = intent->second->decision_timestamp;
        }
    }
    return result;
}
