#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <unordered_set>
#include <vector>

#include "fill.h"
#include "tracked_order.h"
#include "order_update.h"


/**************************************************************************************
 * Type    : OrderManager
 * Purpose : Single source of truth for locally known order lifecycle state
 *
 * OrderManager never changes Account/positions. Fill events are validated/tracked here
 * and are then applied separately to Account by the runtime/backtester.
 **************************************************************************************/
class OrderManager {
private:
    std::unordered_map<OrderID, TrackedOrder> orders_;
    std::unordered_set<FillID> processed_fill_ids_;
    double quantity_epsilon_ = 1e-12;

    static bool transitionAllowed(ExecutionOrderStatus from, ExecutionOrderStatus to)
    {
        if (from == to)
            return true;

        switch (from) {
        case ExecutionOrderStatus::Created:
            return to == ExecutionOrderStatus::Submitted;
        case ExecutionOrderStatus::Submitted:
            return to == ExecutionOrderStatus::Accepted ||
                   to == ExecutionOrderStatus::PartiallyFilled ||
                   to == ExecutionOrderStatus::Filled ||
                   to == ExecutionOrderStatus::Canceled ||
                   to == ExecutionOrderStatus::Rejected;
        case ExecutionOrderStatus::Accepted:
            return to == ExecutionOrderStatus::PartiallyFilled ||
                   to == ExecutionOrderStatus::Filled ||
                   to == ExecutionOrderStatus::Canceled ||
                   to == ExecutionOrderStatus::Rejected;
        case ExecutionOrderStatus::PartiallyFilled:
            return to == ExecutionOrderStatus::Filled ||
                   to == ExecutionOrderStatus::Canceled;
        case ExecutionOrderStatus::Filled:
        case ExecutionOrderStatus::Canceled:
        case ExecutionOrderStatus::Rejected:
            return false;
        }

        return false;
    }

    TrackedOrder& require(OrderID orderId)
    {
        const auto it = orders_.find(orderId);
        if (it == orders_.end())
            throw std::invalid_argument("Unknown order id");
        return it->second;
    }

public:
    explicit OrderManager(double quantityEpsilon = 1e-12)
        : quantity_epsilon_(quantityEpsilon)
    {
        if (!std::isfinite(quantity_epsilon_) || quantity_epsilon_ < 0.0)
            throw std::invalid_argument("Quantity epsilon must be finite and non-negative");
    }

    void track(const ExecutionOrder& request)
    {
        if (request.order_id == 0)
            throw std::invalid_argument("Tracked order id must be non-zero");
        if (!orders_.emplace(request.order_id, TrackedOrder(request)).second)
            throw std::invalid_argument("Order id already tracked");
    }

    /**************************************************************************************
     * Purpose : Restore tracked order/fill-id state without replaying exchange side effects
     **************************************************************************************/
    void restore(
        const std::vector<TrackedOrder>& orders,
        const std::vector<FillID>& processedFillIds
    )
    {
        std::unordered_map<OrderID, TrackedOrder> restoredOrders;
        std::unordered_set<FillID> restoredFillIds;

        for (const TrackedOrder& order : orders) {
            if (order.request.order_id == 0)
                throw std::invalid_argument("Restored order id must be non-zero");
            if (!std::isfinite(order.filled_quantity) || order.filled_quantity < 0.0 ||
                order.filled_quantity > order.request.quantity + quantity_epsilon_)
                throw std::invalid_argument("Invalid restored filled order quantity");
            if (!restoredOrders.emplace(order.request.order_id, order).second)
                throw std::invalid_argument("Duplicate restored order id");
        }

        for (const FillID fillId : processedFillIds) {
            if (fillId == 0)
                throw std::invalid_argument("Restored processed fill id must be non-zero");
            if (!restoredFillIds.insert(fillId).second)
                throw std::invalid_argument("Duplicate restored processed fill id");
        }

        orders_ = std::move(restoredOrders);
        processed_fill_ids_ = std::move(restoredFillIds);
    }

    void markSubmitted(OrderID orderId, Timestamp timestamp)
    {
        TrackedOrder& order = require(orderId);
        if (!transitionAllowed(order.status, ExecutionOrderStatus::Submitted))
            throw std::logic_error("Invalid order transition to Submitted");

        order.status = ExecutionOrderStatus::Submitted;
        order.updated_at = timestamp;
    }

    void markCancelRequested(OrderID orderId, Timestamp timestamp)
    {
        TrackedOrder& order = require(orderId);
        if (!order.isOpen())
            throw std::logic_error("Cannot cancel terminal order");

        order.cancel_requested = true;
        order.updated_at = timestamp;
    }

    void onOrderUpdate(const OrderUpdate& update)
    {
        TrackedOrder& order = require(update.order_id);
        if (!transitionAllowed(order.status, update.status))
            throw std::logic_error("Invalid order status transition");

        order.status = update.status;
        order.updated_at = update.timestamp;

        if (!update.exchange_order_id.empty())
            order.exchange_order_id = update.exchange_order_id;
        if (!update.message.empty())
            order.last_message = update.message;
    }

    bool onFill(const Fill& fill)
    {
        fill.validate();

        if (fill.fill_id != 0 && !processed_fill_ids_.insert(fill.fill_id).second)
            return false; // Idempotent replay protection for repeated exchange events.

        TrackedOrder& order = require(fill.order_id);
        if (order.status == ExecutionOrderStatus::Rejected)
            throw std::logic_error("Rejected order cannot receive fills");
        if (fill.strategy_id != order.request.strategy_id ||
            fill.coin != order.request.coin ||
            fill.side != order.request.side)
            throw std::logic_error("Fill does not match tracked order");

        const double newFilledQuantity = order.filled_quantity + fill.quantity;
        if (newFilledQuantity > order.request.quantity + quantity_epsilon_)
            throw std::logic_error("Order filled quantity exceeds requested quantity");

        order.filled_quantity = std::min(newFilledQuantity, order.request.quantity);
        order.updated_at = fill.timestamp;

        if (order.remainingQuantity() <= quantity_epsilon_)
            order.status = ExecutionOrderStatus::Filled;
        else if (order.status != ExecutionOrderStatus::Canceled)
            order.status = ExecutionOrderStatus::PartiallyFilled;

        return true;
    }

    const TrackedOrder* find(OrderID orderId) const
    {
        const auto it = orders_.find(orderId);
        return it == orders_.end() ? nullptr : &it->second;
    }

    const std::unordered_map<OrderID, TrackedOrder>& orders() const
    {
        return orders_;
    }

    std::vector<FillID> processedFillIds() const
    {
        return {processed_fill_ids_.begin(), processed_fill_ids_.end()};
    }

    std::vector<OrderID> openOrderIds() const
    {
        std::vector<OrderID> result;
        for (const auto& [orderId, order] : orders_) {
            if (order.isOpen())
                result.push_back(orderId);
        }
        return result;
    }

    std::vector<OrderID> openOrderIds(StrategyID strategyId, const Coin& coin) const
    {
        std::vector<OrderID> result;
        for (const auto& [orderId, order] : orders_) {
            if (order.isOpen() &&
                order.request.strategy_id == strategyId &&
                order.request.coin == coin)
                result.push_back(orderId);
        }
        return result;
    }

    std::vector<OrderID> cancelableOpenOrderIds(StrategyID strategyId, const Coin& coin) const
    {
        std::vector<OrderID> result;
        for (const auto& [orderId, order] : orders_) {
            if (order.isOpen() && !order.cancel_requested &&
                order.request.strategy_id == strategyId &&
                order.request.coin == coin)
                result.push_back(orderId);
        }
        return result;
    }

    bool hasOpenOrder(StrategyID strategyId, const Coin& coin) const
    {
        for (const auto& [orderId, order] : orders_) {
            (void)orderId;
            if (order.isOpen() &&
                order.request.strategy_id == strategyId &&
                order.request.coin == coin)
                return true;
        }
        return false;
    }

    double pendingSignedQuantity(StrategyID strategyId, const Coin& coin) const
    {
        double quantity = 0.0;
        for (const auto& [orderId, order] : orders_) {
            (void)orderId;
            if (order.request.strategy_id == strategyId && order.request.coin == coin)
                quantity += order.pendingSignedQuantity();
        }
        return quantity;
    }
};
