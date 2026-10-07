#pragma once

#include <cmath>
#include <set>
#include <stdexcept>
#include <utility>
#include <vector>

#include "execution_order.h"
#include "order_manager.h"
#include "position_state.h"
#include "target_resolution.h"

// Exchange commands required to move current/pending state toward a target
//
// Cancellations are deliberately separate from submissions. If an existing open order
// conflicts with a newer target, cancel it first and wait for the exchange confirmation
// before creating its replacement. This avoids crossed/duplicate live orders.
struct ExecutionPlan {
    std::vector<OrderID> order_ids_to_cancel;
    std::vector<ExecutionOrder> orders_to_submit;

    bool empty() const
    {
        return order_ids_to_cancel.empty() && orders_to_submit.empty();
    }
};

// Final execution-level invariant check before an order is submitted
//
// Portfolio concentration/leverage limits belong to RiskConstraints and are already
// reflected in targetQuantity. This guard does not optimize or resize the portfolio.
// It only guarantees that a new order moves effective filled+pending quantity toward
// the already-approved target without crossing/overshooting it.
class OrderTargetGuard {
private:
    double quantity_epsilon_ = 1e-12;

public:
    explicit OrderTargetGuard(double quantityEpsilon = 1e-12)
        : quantity_epsilon_(quantityEpsilon)
    {
        if (!std::isfinite(quantity_epsilon_) || quantity_epsilon_ < 0.0)
            throw std::invalid_argument("Quantity epsilon must be finite and non-negative");
    }

    bool allows(
        const ExecutionOrder& order,
        double targetQuantity,
        double currentQuantity,
        double pendingSignedQuantity
    ) const
    {
        if (!std::isfinite(targetQuantity) ||
            !std::isfinite(currentQuantity) ||
            !std::isfinite(pendingSignedQuantity))
            return false;

        const double effectiveQuantity = currentQuantity + pendingSignedQuantity;
        const double requiredDelta = targetQuantity - effectiveQuantity;
        const double proposedDelta = order.signedQuantity();

        if (std::abs(requiredDelta) <= quantity_epsilon_)
            return false; // Already covered by fills + pending orders.

        if (requiredDelta > 0.0 && proposedDelta <= 0.0)
            return false;
        if (requiredDelta < 0.0 && proposedDelta >= 0.0)
            return false;

        // Never cross the approved target. Smaller orders remain valid for venues that
        // impose quantity/notional limits; another execution cycle can submit the rest.
        return std::abs(proposedDelta) <= std::abs(requiredDelta) + quantity_epsilon_;
    }

    void validate(
        const ExecutionOrder& order,
        double targetQuantity,
        double currentQuantity,
        double pendingSignedQuantity
    ) const
    {
        if (!allows(order, targetQuantity, currentQuantity, pendingSignedQuantity))
            throw std::logic_error("Execution order would duplicate or overshoot approved target");
    }
};

// Convert strategy quantity targets into safe executable quantity deltas
//
// Effective quantity = filled virtual position + remaining quantity of open orders.
// This prevents repeated execution cycles from submitting the same delta again while an
// exchange order is still pending.
//
// If a target changes while an order is open, the old order is canceled first. A later
// cycle may submit the replacement after the cancel is confirmed; opposing live orders
// are never intentionally stacked for the same strategy/asset.
class QuantityOrderBuilder {
private:
    double quantity_epsilon_ = 1e-12;
    OrderTargetGuard target_guard_;

public:
    explicit QuantityOrderBuilder(double quantityEpsilon = 1e-12)
        : quantity_epsilon_(quantityEpsilon),
          target_guard_(quantityEpsilon)
    {
        if (!std::isfinite(quantity_epsilon_) || quantity_epsilon_ < 0.0)
            throw std::invalid_argument("Quantity epsilon must be finite and non-negative");
    }

    ExecutionPlan createMarketPlan(
        const std::vector<StrategyExecutionTarget>& targets,
        const std::vector<VirtualPositionState>& currentStrategyPositions,
        const OrderManager& orderManager,
        Timestamp createdAt,
        Timestamp activeFrom,
        OrderID& nextOrderId
    ) const
    {
        if (targets.size() != currentStrategyPositions.size())
            throw std::invalid_argument("Strategy target/current-position sizes do not match");

        ExecutionPlan plan;

        for (std::size_t i = 0; i < targets.size(); ++i) {
            const auto& target = targets[i];
            const auto& current = currentStrategyPositions[i];

            // Canonical coin order is part of deterministic OrderID assignment.
            // The distributed planner reconstructs unordered maps from transport, so
            // relying on unordered iteration can swap OrderID<->coin attribution
            // across otherwise equivalent processes.
            std::set<Coin> coins;
            for (const auto& [coin, quantity] : target.positions.values()) {
                (void)quantity;
                coins.insert(coin);
            }
            for (const auto& [coin, quantity] : current.values()) {
                (void)quantity;
                coins.insert(coin);
            }
            for (const auto& [orderId, order] : orderManager.orders()) {
                (void)orderId;
                if (order.isOpen() && order.request.strategy_id == target.strategy_id)
                    coins.insert(order.request.coin);
            }

            for (const Coin& coin : coins) {
                const double currentQuantity = current.get(coin);
                const double pendingQuantity =
                    orderManager.pendingSignedQuantity(target.strategy_id, coin);
                const double effectiveQuantity = currentQuantity + pendingQuantity;
                const double targetQuantity = target.positions.get(coin);
                const double delta = targetQuantity - effectiveQuantity;

                if (std::abs(delta) <= quantity_epsilon_)
                    continue; // Existing fills + pending orders already cover this target.

                if (orderManager.hasOpenOrder(target.strategy_id, coin)) {
                    // The existing order represents an older target. Cancel it first and
                    // wait for the terminal update before submitting a replacement.
                    const auto openIds = orderManager.cancelableOpenOrderIds(target.strategy_id, coin);
                    plan.order_ids_to_cancel.insert(
                        plan.order_ids_to_cancel.end(),
                        openIds.begin(),
                        openIds.end()
                    );
                    continue;
                }

                if (nextOrderId == 0)
                    ++nextOrderId;

                ExecutionOrder order(
                    nextOrderId++,
                    target.strategy_id,
                    createdAt,
                    activeFrom,
                    coin,
                    delta > 0.0 ? OrderSide::Buy : OrderSide::Sell,
                    std::abs(delta)
                );

                target_guard_.validate(
                    order,
                    targetQuantity,
                    currentQuantity,
                    pendingQuantity
                );

                plan.orders_to_submit.push_back(std::move(order));
            }
        }

        return plan;
    }
};
