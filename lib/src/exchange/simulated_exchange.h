#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exchange.h"


/**************************************************************************************
 * Type    : SimulatedExchange
 * Purpose : Minimal market-order exchange used by the current backtester
 *
 * Current convention:
 *   - only market orders are supported;
 *   - an order fills fully at the OPEN of its active_from timestamp;
 *   - commission is charged as abs(quantity * fillPrice) * commissionRate;
 *   - no slippage/partial fills yet.
 *
 * Unlike the original simulator, lifecycle updates and fills are emitted through the
 * same ordered ExchangeEvent stream. This mirrors the boundary a live adapter will use.
 **************************************************************************************/
class SimulatedExchange final : public Exchange {
private:
    double commission_rate_ = 0.0;
    FillID next_fill_id_ = 1;

    std::unordered_map<OrderID, ExecutionOrder> active_orders_;
    std::vector<ExchangeEvent> events_;

public:
    explicit SimulatedExchange(double commissionRate = 0.0)
        : commission_rate_(commissionRate)
    {
        if (!std::isfinite(commission_rate_) || commission_rate_ < 0.0)
            throw std::invalid_argument("Commission rate must be finite and non-negative");
    }

    void submitOrder(const ExecutionOrder& order) override
    {
        if (order.order_id == 0)
            throw std::invalid_argument("Order id must be non-zero");
        if (active_orders_.find(order.order_id) != active_orders_.end())
            throw std::invalid_argument("Order id already active");

        active_orders_.emplace(order.order_id, order);
        events_.emplace_back(OrderUpdate(
            order.order_id,
            order.created_at,
            ExecutionOrderStatus::Accepted,
            "sim-" + std::to_string(order.order_id)
        ));
    }

    void cancelOrderAt(OrderID orderId, Timestamp timestamp)
    {
        const auto it = active_orders_.find(orderId);
        if (it == active_orders_.end())
            return;

        events_.emplace_back(OrderUpdate(
            orderId,
            timestamp,
            ExecutionOrderStatus::Canceled,
            "sim-" + std::to_string(orderId)
        ));
        active_orders_.erase(it);
    }

    void cancelOrder(OrderID orderId) override
    {
        cancelOrderAt(orderId, 0);
    }

    /**************************************************************************************
     * Purpose : Process market orders that become active at this bar open
     **************************************************************************************/
    void processOpen(Timestamp ts, const CoinBarMap& bars)
    {
        // Fill IDs are part of the externally observable exchange contract. Do not let
        // unordered_map bucket order decide which simultaneously-active order receives
        // the next FillID. Canonicalize by local OrderID so in-process and distributed
        // replay produce identical lifecycle identifiers.
        std::vector<OrderID> executableOrderIds;
        executableOrderIds.reserve(active_orders_.size());

        for (const auto& [orderId, order] : active_orders_) {
            if (order.active_from <= ts && bars.find(order.coin) != bars.end())
                executableOrderIds.push_back(orderId);
        }

        std::sort(executableOrderIds.begin(), executableOrderIds.end());

        for (OrderID orderId : executableOrderIds) {
            const auto orderIt = active_orders_.find(orderId);
            if (orderIt == active_orders_.end())
                continue;

            const ExecutionOrder& order = orderIt->second;
            const auto barIt = bars.find(order.coin);
            if (barIt == bars.end())
                continue;

            const double fillPrice = barIt->second.open;
            if (!std::isfinite(fillPrice) || fillPrice <= 0.0)
                throw std::runtime_error("Invalid simulated market fill price");

            Fill fill;
            fill.fill_id = next_fill_id_++;
            fill.order_id = order.order_id;
            fill.strategy_id = order.strategy_id;
            fill.timestamp = ts;
            fill.coin = order.coin;
            fill.side = order.side;
            fill.quantity = order.quantity;
            fill.price = fillPrice;
            fill.commission = std::abs(order.quantity * fillPrice) * commission_rate_;
            fill.validate();

            // Preserve exchange event ordering: execution first, terminal status second.
            events_.emplace_back(std::move(fill));
            events_.emplace_back(OrderUpdate(
                order.order_id,
                ts,
                ExecutionOrderStatus::Filled,
                "sim-" + std::to_string(order.order_id)
            ));
            active_orders_.erase(orderIt);
        }
    }

    std::vector<ExchangeEvent> drainEvents() override
    {
        std::vector<ExchangeEvent> result;
        result.swap(events_);
        return result;
    }

    const std::unordered_map<OrderID, ExecutionOrder>& activeOrders() const
    {
        return active_orders_;
    }

    double commissionRate() const
    {
        return commission_rate_;
    }
};
