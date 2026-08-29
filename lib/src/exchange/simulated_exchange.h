#pragma once

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

    void cancelOrder(OrderID orderId) override
    {
        const auto it = active_orders_.find(orderId);
        if (it == active_orders_.end())
            return;

        events_.emplace_back(OrderUpdate(
            orderId,
            0,
            ExecutionOrderStatus::Canceled,
            "sim-" + std::to_string(orderId)
        ));
        active_orders_.erase(it);
    }

    /**************************************************************************************
     * Purpose : Process market orders that become active at this bar open
     **************************************************************************************/
    void processOpen(Timestamp ts, const CoinBarMap& bars)
    {
        for (auto it = active_orders_.begin(); it != active_orders_.end(); ) {
            const ExecutionOrder& order = it->second;
            if (order.active_from > ts) {
                ++it;
                continue;
            }

            const auto barIt = bars.find(order.coin);
            if (barIt == bars.end()) {
                ++it;
                continue;
            }

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
            it = active_orders_.erase(it);
        }
    }

    std::vector<ExchangeEvent> drainEvents() override
    {
        std::vector<ExchangeEvent> result;
        result.swap(events_);
        return result;
    }

    double commissionRate() const
    {
        return commission_rate_;
    }
};
