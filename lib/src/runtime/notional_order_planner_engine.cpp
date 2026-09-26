#include "notional_order_planner_engine.h"

#include "live_execution_identity.h"

#include <cmath>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>


NotionalOrderPlannerEngine::NotionalOrderPlannerEngine(double notionalEpsilonUsd)
    : notional_epsilon_usd_(notionalEpsilonUsd)
{
    if (!std::isfinite(notional_epsilon_usd_) || notional_epsilon_usd_ < 0.0)
        throw std::invalid_argument("Notional epsilon must be finite and non-negative");
}

NotionalOrderPlannerResult NotionalOrderPlannerEngine::createPlan(
    const std::vector<StrategyID>& strategyIds,
    const StrategyPositionSnapshot& strategyPositions,
    const OrderManager& orderManager,
    Timestamp decisionTimestamp,
    std::uint64_t stateRevision,
    const ExecutionReferencePrices& closes,
    const DecisionBatch& decisions,
    OrderID nextOrderId) const
{
    if (decisionTimestamp == 0 || decisions.decision_timestamp != decisionTimestamp)
        throw std::invalid_argument("Notional planner decision timestamp mismatch");
    if (stateRevision == 0)
        throw std::invalid_argument("Notional planner state revision must be non-zero");
    if (nextOrderId == 0)
        throw std::invalid_argument("Notional planner next order id must be non-zero");

    std::unordered_set<StrategyID> configuredIds;
    configuredIds.reserve(strategyIds.size());
    for (const StrategyID strategyId : strategyIds) {
        if (!configuredIds.insert(strategyId).second)
            throw std::invalid_argument("Notional planner contains duplicate configured strategy id");
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
    monetaryTargets.reserve(strategyIds.size());

    NotionalOrderPlannerResult result;
    result.next_order_id = nextOrderId;

    for (const StrategyID strategyId : strategyIds) {
        const auto positionIt = strategyPositions.find(strategyId);
        const VirtualPositionState emptyPositions;
        const VirtualPositionState& currentPositions =
            positionIt == strategyPositions.end() ? emptyPositions : positionIt->second;

        TargetPortfolio target;

        // Missing decisions mean HOLD, so begin from the actually filled quantities
        // valued at the common close(T) reference.
        for (const auto& [coin, quantity] : currentPositions.values()) {
            if (!closes.contains(coin))
                throw std::runtime_error("Missing close(T) for held strategy asset");
            target.set(coin, quantity * closes.get(coin));
        }

        const auto intentIt = intents.find(strategyId);
        if (intentIt != intents.end()) {
            const StrategyDecisionIntent& intent = *intentIt->second;

            for (const auto& [coin, notional] : intent.target_notional_usd) {
                if (intent.decisions.find(coin) == intent.decisions.end())
                    throw std::invalid_argument(
                        "Decision contains USD target without matching rebalance decision");
                if (!std::isfinite(notional))
                    throw std::invalid_argument("Decision USD target must be finite");
            }

            for (const auto& [coin, decision] : intent.decisions) {
                const auto notionalIt = intent.target_notional_usd.find(coin);
                if (notionalIt == intent.target_notional_usd.end())
                    throw std::invalid_argument(
                        "LIVE decision is missing explicit target_notional_usd for " + coin);

                const double targetUsd = notionalIt->second;
                if (!std::isfinite(targetUsd))
                    throw std::invalid_argument("LIVE target_notional_usd must be finite");
                if (decision.action == RebalanceAction::Hold)
                    throw std::invalid_argument("LIVE DecisionBatch must not contain explicit HOLD entries");
                if (decision.action == RebalanceAction::Flat && targetUsd != 0.0)
                    throw std::invalid_argument("FLAT decision must carry zero target_notional_usd");

                target.set(coin, targetUsd);
            }
        }

        monetaryTargets.push_back(target);

        std::set<Coin> coins;
        for (const auto& [coin, exposure] : target.values()) {
            (void)exposure;
            coins.insert(coin);
        }
        for (const auto& [coin, quantity] : currentPositions.values()) {
            (void)quantity;
            coins.insert(coin);
        }
        for (const auto& [orderId, tracked] : orderManager.orders()) {
            (void)orderId;
            if (tracked.isOpen() && tracked.request.strategy_id == strategyId)
                coins.insert(tracked.request.coin);
        }

        for (const Coin& coin : coins) {
            if (!closes.contains(coin))
                throw std::runtime_error("Missing close(T) for notional planning asset: " + coin);

            const double close = closes.get(coin);
            if (!std::isfinite(close) || close <= 0.0)
                throw std::runtime_error("Invalid close(T) for notional planning asset: " + coin);

            const double currentUsd = currentPositions.get(coin) * close;
            const double pendingUsd =
                orderManager.pendingSignedQuantity(strategyId, coin) * close;
            const double targetUsd = target.get(coin);
            const double deltaUsd = targetUsd - currentUsd - pendingUsd;

            if (std::abs(deltaUsd) <= notional_epsilon_usd_)
                continue;

            if (orderManager.hasOpenOrder(strategyId, coin)) {
                const auto openIds = orderManager.cancelableOpenOrderIds(strategyId, coin);
                result.cancel_order_ids.insert(
                    result.cancel_order_ids.end(), openIds.begin(), openIds.end());
                continue;
            }

            if (result.next_order_id == 0)
                ++result.next_order_id;

            PlannedNotionalOrder order;
            order.order_id = result.next_order_id++;
            order.economic_order_id = LiveExecutionIdentity::plannedEconomicOrder(
                decisionTimestamp, stateRevision, order.order_id);
            order.strategy_id = strategyId;
            order.created_at = decisionTimestamp;
            order.decision_timestamp = decisionTimestamp;
            order.state_revision = stateRevision;
            order.coin = coin;
            order.side = deltaUsd > 0.0 ? OrderSide::Buy : OrderSide::Sell;
            order.reference_close = close;
            order.target_notional_usd = targetUsd;
            order.current_notional_usd = currentUsd;
            order.pending_notional_usd = pendingUsd;
            order.delta_notional_usd = deltaUsd;
            order.notional_usd = std::abs(deltaUsd);
            result.submit_orders.push_back(std::move(order));
        }
    }

    result.global_target = portfolio_aggregator_.aggregate(monetaryTargets);
    return result;
}
