#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exchange.h"


// Deterministic exchange used by the current backtester
//
// Current convention:
// - market orders fill fully at the OPEN of their active_from timestamp;
// - research stop entries match once the following bar is complete, or expire;
// - commission is charged as abs(quantity * fillPrice) * commissionRate;
// - split fills are available for lifecycle validation.
//
// Unlike the original simulator, lifecycle updates and fills are emitted through the
// same ordered ExchangeEvent stream. This mirrors the boundary a live adapter will use.
class SimulatedExchange final : public Exchange {
private:
    static void validateStopEntry(const ExecutionOrder& order)
    {
        order.validateConditional();
    }
    double commission_rate_ = 0.0;
    FillID next_fill_id_ = 1;
    bool reject_next_order_ = false;
    std::string reject_next_order_message_ = "CHAOS_REJECT";
    double split_next_fill_fraction_ = 0.0;

    std::unordered_map<OrderID, ExecutionOrder> active_orders_;
    std::vector<ExchangeEvent> events_;

    void emitFill(const ExecutionOrder& order, Timestamp ts, double fillPrice, double fillQuantity)
    {
        const auto makeFill = [&](double quantity) {
            Fill fill;
            fill.fill_id = next_fill_id_++;
            fill.order_id = order.order_id;
            fill.strategy_id = order.strategy_id;
            fill.timestamp = ts;
            fill.coin = order.coin;
            fill.side = order.side;
            fill.quantity = quantity;
            fill.price = fillPrice;
            fill.commission = std::abs(quantity * fillPrice) * commission_rate_;
            fill.validate();
            return fill;
        };

        if (split_next_fill_fraction_ > 0.0) {
            const double firstQuantity = fillQuantity * split_next_fill_fraction_;
            const double secondQuantity = fillQuantity - firstQuantity;
            split_next_fill_fraction_ = 0.0;

            if (firstQuantity <= 0.0 || secondQuantity <= 0.0)
                throw std::runtime_error("Invalid chaos partial-fill quantities");

            events_.emplace_back(makeFill(firstQuantity));
            events_.emplace_back(makeFill(secondQuantity));
        }
        else {
            events_.emplace_back(makeFill(fillQuantity));
        }

        // Preserve exchange event ordering: execution first, terminal status second.
        // A gap spends the entire reference notional with fewer units. Cancel
        // the unused quantity cap so it cannot become a second entry.
        events_.emplace_back(OrderUpdate(
            order.order_id,
            ts,
            fillQuantity < order.quantity ? ExecutionOrderStatus::Canceled : ExecutionOrderStatus::Filled,
            "sim-" + std::to_string(order.order_id)
        ));
    }

public:
    bool supportsStopEntries() const override { return true; }
    bool supportsShortBrackets() const override { return true; }
    explicit SimulatedExchange(double commissionRate = 0.0)
        : commission_rate_(commissionRate)
    {
        if (!std::isfinite(commission_rate_) || commission_rate_ < 0.0)
            throw std::invalid_argument("Commission rate must be finite and non-negative");
    }

    void submitOrder(const ExecutionOrder& order) override
    {
        validateStopEntry(order);
        if (order.order_id == 0)
            throw std::invalid_argument("Order id must be non-zero");
        if (active_orders_.find(order.order_id) != active_orders_.end())
            throw std::invalid_argument("Order id already active");

        if (reject_next_order_) {
            reject_next_order_ = false;
            events_.emplace_back(OrderUpdate(
                order.order_id,
                order.entry_stop_price != 0.0 ? order.active_from : order.created_at,
                ExecutionOrderStatus::Rejected,
                "sim-" + std::to_string(order.order_id),
                reject_next_order_message_
            ));
            return;
        }

        active_orders_.emplace(order.order_id, order);
        events_.emplace_back(OrderUpdate(
            order.order_id,
            order.entry_stop_price != 0.0 ? order.active_from : order.created_at,
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

    // Process market orders that become active at this bar open
    void processOpen(Timestamp ts, const CoinBarMap& bars)
    {
        // Fill IDs are part of the externally observable exchange contract. Do not let
        // unordered_map bucket order decide which simultaneously-active order receives
        // the next FillID. Canonicalize by local OrderID so in-process and distributed
        // replay produce identical lifecycle identifiers.
        std::vector<OrderID> executableOrderIds;
        executableOrderIds.reserve(active_orders_.size());

        for (const auto& [orderId, order] : active_orders_) {
            if (order.entry_stop_price == 0.0 && order.parent_order_id == 0 &&
                order.active_from <= ts && bars.find(order.coin) != bars.end())
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

            emitFill(order, ts, fillPrice, order.quantity);
            active_orders_.erase(orderIt);
        }
    }

    std::vector<ExchangeEvent> drainEvents() override
    {
        std::vector<ExchangeEvent> result;
        result.swap(events_);
        return result;
    }

    // Called only once the complete following bar is visible. Entry-bar high is
    // never read at the open. An untriggered (or missing-asset) order expires here.
    void processStopEntriesAtClose(Timestamp ts, const CoinBarMap& bars)
    {
        std::vector<OrderID> eligible;
        for (const auto& [id, order] : active_orders_) {
            if (order.entry_stop_price != 0.0 && order.active_from <= ts && ts > order.created_at)
                eligible.push_back(id);
        }
        std::sort(eligible.begin(), eligible.end());
        for (OrderID id : eligible) {
            const auto it = active_orders_.find(id);
            const ExecutionOrder& order = it->second;
            const auto bar = bars.find(order.coin);
            if (bar == bars.end()) {
                cancelOrderAt(id, ts);
                continue;
            }
            if (!std::isfinite(bar->second.open) || bar->second.open <= 0.0 ||
                !std::isfinite(bar->second.high) || bar->second.high < bar->second.open)
                throw std::runtime_error("Invalid bar for simulated stop matching");
            const bool shortEntry = order.side == OrderSide::Sell;
            if (shortEntry && (!std::isfinite(bar->second.low) || bar->second.low <= 0.0 ||
                              bar->second.low > bar->second.open))
                throw std::runtime_error("Invalid low for simulated short stop matching");
            if (shortEntry ? bar->second.low > order.entry_stop_price
                           : bar->second.high < order.entry_stop_price) {
                cancelOrderAt(id, ts);
                continue;
            }
            const double price = shortEntry ? std::min(order.entry_stop_price, bar->second.open)
                                           : std::max(order.entry_stop_price, bar->second.open);
            const double quantity = shortEntry ? order.quantity
                : order.quantity * (order.entry_stop_price / price);
            emitFill(order, ts, price, quantity);
            active_orders_.erase(it);
        }
    }

    void processProtectiveStopsAtClose(Timestamp ts, const CoinBarMap& bars)
    {
        std::vector<OrderID> eligible;
        for (const auto& [id, order] : active_orders_) {
            if (order.parent_order_id != 0 && order.active_from <= ts)
                eligible.push_back(id);
        }
        std::sort(eligible.begin(), eligible.end());
        for (OrderID id : eligible) {
            const auto it = active_orders_.find(id);
            const ExecutionOrder& order = it->second;
            const auto bar = bars.find(order.coin);
            if (bar == bars.end())
                continue;
            if (!std::isfinite(bar->second.open) || bar->second.open <= 0.0 ||
                !std::isfinite(bar->second.high) || bar->second.high < bar->second.open ||
                !std::isfinite(bar->second.close))
                throw std::runtime_error("Invalid bar for protective stop matching");
            // Preserve the research OHLC assumption on the entry bar:
            // bullish open-low-high-close can trigger entry then cover;
            // bearish open-high-low-close cannot cover after the low entry.
            if (ts == order.created_at && bar->second.close < bar->second.open)
                continue;
            if (bar->second.high >= order.protective_stop_price) {
                emitFill(order, ts, std::max(order.protective_stop_price, bar->second.open), order.quantity);
                active_orders_.erase(it);
            }
        }
    }

    void configureRejectNextOrder(std::string message = "CHAOS_REJECT")
    {
        reject_next_order_ = true;
        reject_next_order_message_ = std::move(message);
    }

    void configureSplitNextFill(double fraction)
    {
        if (!std::isfinite(fraction) || fraction <= 0.0 || fraction >= 1.0)
            throw std::invalid_argument("Split-fill fraction must be between 0 and 1");
        split_next_fill_fraction_ = fraction;
    }

    // Restore exchange-owned durable state after a process/container restart
    //
    // Recovery must not recreate accepted/fill lifecycle events. Those are handled by
    // the service durable outbox. This method restores only the exchange truth required
    // to continue deterministic execution.
    void restoreState(
        std::unordered_map<OrderID, ExecutionOrder> activeOrders,
        FillID nextFillId
    )
    {
        if (nextFillId == 0)
            throw std::invalid_argument("Restored next FillID must be non-zero");

        for (const auto& [orderId, order] : activeOrders) {
            validateStopEntry(order);
            if (orderId == 0 || order.order_id != orderId)
                throw std::invalid_argument("Restored active order id is invalid");
            if (order.coin.empty() || !std::isfinite(order.quantity) || order.quantity <= 0.0)
                throw std::invalid_argument("Restored active order is invalid");
        }

        active_orders_ = std::move(activeOrders);
        next_fill_id_ = nextFillId;
        events_.clear();
    }

    FillID nextFillId() const
    {
        return next_fill_id_;
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
