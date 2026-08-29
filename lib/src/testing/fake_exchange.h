#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "exchange.h"
#include "exchange_snapshot.h"


/**************************************************************************************
 * Type    : FakeExchange
 * Purpose : Deterministic asynchronous exchange used by local live-style tests
 *
 * submitOrder/cancelOrder only record commands. Tests explicitly choose when the
 * exchange accepts/rejects/cancels/fills an order, so partial and delayed execution can
 * be reproduced without sleeping, threads, sockets or a real API.
 *
 * FakeExchange also owns an exchange-side cash/position/open-order view. snapshot()
 * therefore exercises the same recovery/reconciliation boundary a future LiveExchange
 * adapter will have to populate from its API.
 **************************************************************************************/
class FakeExchange final : public Exchange {
private:
    struct FakeOpenOrder {
        ExecutionOrder request;
        double filled_quantity = 0.0;
        bool accepted = false;
        bool cancel_requested = false;
        std::string exchange_order_id;
    };

    double cash_ = 0.0;
    double commission_rate_ = 0.0;
    double quantity_epsilon_ = 1e-12;
    FillID next_fill_id_ = 1;

    std::unordered_map<Coin, double> positions_;
    std::unordered_map<OrderID, FakeOpenOrder> active_orders_;
    std::unordered_map<OrderID, std::size_t> cancel_request_count_;
    std::vector<ExchangeEvent> events_;
    std::optional<Fill> last_fill_;
    std::size_t submit_count_ = 0;

    FakeOpenOrder& require(OrderID orderId)
    {
        const auto it = active_orders_.find(orderId);
        if (it == active_orders_.end())
            throw std::invalid_argument("Unknown fake exchange order id");
        return it->second;
    }

    void addPosition(const Coin& coin, double quantity)
    {
        const double newQuantity = position(coin) + quantity;
        if (std::abs(newQuantity) <= quantity_epsilon_) {
            positions_.erase(coin);
            return;
        }
        positions_[coin] = newQuantity;
    }

public:
    explicit FakeExchange(
        double initialCash,
        double commissionRate = 0.0,
        double quantityEpsilon = 1e-12
    )
        : cash_(initialCash),
          commission_rate_(commissionRate),
          quantity_epsilon_(quantityEpsilon)
    {
        if (!std::isfinite(cash_))
            throw std::invalid_argument("Fake exchange cash must be finite");
        if (!std::isfinite(commission_rate_) || commission_rate_ < 0.0)
            throw std::invalid_argument("Fake exchange commission must be finite and non-negative");
        if (!std::isfinite(quantity_epsilon_) || quantity_epsilon_ < 0.0)
            throw std::invalid_argument("Fake exchange quantity epsilon must be finite and non-negative");
    }

    /**************************************************************************************
     * Purpose : Record submission without immediately ACKing/filling it
     **************************************************************************************/
    void submitOrder(const ExecutionOrder& order) override
    {
        if (order.order_id == 0)
            throw std::invalid_argument("Order id must be non-zero");
        if (active_orders_.find(order.order_id) != active_orders_.end())
            throw std::invalid_argument("Order id already active at fake exchange");

        FakeOpenOrder state;
        state.request = order;
        state.exchange_order_id = "fake-" + std::to_string(order.order_id);
        active_orders_.emplace(order.order_id, std::move(state));
        ++submit_count_;
    }

    /**************************************************************************************
     * Purpose : Record a cancel command; confirmation remains explicitly controlled
     **************************************************************************************/
    void cancelOrder(OrderID orderId) override
    {
        auto it = active_orders_.find(orderId);
        if (it == active_orders_.end())
            return;

        it->second.cancel_requested = true;
        ++cancel_request_count_[orderId];
    }

    void acceptOrder(OrderID orderId, Timestamp timestamp)
    {
        FakeOpenOrder& order = require(orderId);
        if (order.accepted)
            throw std::logic_error("Fake order already accepted");

        order.accepted = true;
        events_.emplace_back(OrderUpdate(
            orderId,
            timestamp,
            ExecutionOrderStatus::Accepted,
            order.exchange_order_id
        ));
    }

    void rejectOrder(OrderID orderId, Timestamp timestamp, std::string reason = "fake rejection")
    {
        FakeOpenOrder& order = require(orderId);
        if (order.filled_quantity > quantity_epsilon_)
            throw std::logic_error("Partially filled fake order cannot be rejected");

        events_.emplace_back(OrderUpdate(
            orderId,
            timestamp,
            ExecutionOrderStatus::Rejected,
            order.exchange_order_id,
            std::move(reason)
        ));
        active_orders_.erase(orderId);
    }

    void confirmCancel(OrderID orderId, Timestamp timestamp)
    {
        FakeOpenOrder& order = require(orderId);
        if (!order.cancel_requested)
            throw std::logic_error("Cancel was not requested for fake order");

        events_.emplace_back(OrderUpdate(
            orderId,
            timestamp,
            ExecutionOrderStatus::Canceled,
            order.exchange_order_id
        ));
        active_orders_.erase(orderId);
    }

    /**************************************************************************************
     * Purpose : Emit one partial/final fill and mutate exchange-side account truth
     **************************************************************************************/
    Fill fillOrder(OrderID orderId, double quantity, double price, Timestamp timestamp)
    {
        FakeOpenOrder& order = require(orderId);
        if (!order.accepted)
            throw std::logic_error("Fake order must be accepted before it can fill");
        if (!std::isfinite(quantity) || quantity <= 0.0)
            throw std::invalid_argument("Fake fill quantity must be finite and positive");
        if (!std::isfinite(price) || price <= 0.0)
            throw std::invalid_argument("Fake fill price must be finite and positive");

        const double remaining = order.request.quantity - order.filled_quantity;
        if (quantity > remaining + quantity_epsilon_)
            throw std::invalid_argument("Fake fill exceeds remaining order quantity");

        Fill fill;
        fill.fill_id = next_fill_id_++;
        fill.order_id = orderId;
        fill.strategy_id = order.request.strategy_id;
        fill.timestamp = timestamp;
        fill.coin = order.request.coin;
        fill.side = order.request.side;
        fill.quantity = std::min(quantity, remaining);
        fill.price = price;
        fill.commission = std::abs(fill.quantity * price) * commission_rate_;
        fill.validate();

        const double signedQuantity = fill.signedQuantity();
        cash_ -= signedQuantity * fill.price;
        cash_ -= fill.commission;
        addPosition(fill.coin, signedQuantity);

        order.filled_quantity += fill.quantity;
        last_fill_ = fill;
        events_.emplace_back(fill);

        if (order.request.quantity - order.filled_quantity <= quantity_epsilon_) {
            events_.emplace_back(OrderUpdate(
                orderId,
                timestamp,
                ExecutionOrderStatus::Filled,
                order.exchange_order_id
            ));
            active_orders_.erase(orderId);
        } else {
            events_.emplace_back(OrderUpdate(
                orderId,
                timestamp,
                ExecutionOrderStatus::PartiallyFilled,
                order.exchange_order_id
            ));
        }

        return fill;
    }

    /**************************************************************************************
     * Purpose : Replay the latest FillID to test idempotent reconnect/event handling
     **************************************************************************************/
    void replayLastFill()
    {
        if (!last_fill_)
            throw std::logic_error("No fake fill exists to replay");
        events_.emplace_back(*last_fill_);
    }

    std::vector<ExchangeEvent> drainEvents() override
    {
        std::vector<ExchangeEvent> result;
        result.swap(events_);
        return result;
    }

    ExchangeSnapshot snapshot(Timestamp timestamp) const
    {
        ExchangeSnapshot result;
        result.timestamp = timestamp;
        result.cash = cash_;
        result.positions = positions_;
        result.open_orders.reserve(active_orders_.size());

        for (const auto& [orderId, order] : active_orders_) {
            result.open_orders.push_back({
                orderId,
                order.exchange_order_id,
                order.request.coin,
                order.request.side,
                order.request.quantity,
                order.filled_quantity
            });
        }

        return result;
    }

    double cash() const { return cash_; }

    double position(const Coin& coin) const
    {
        const auto it = positions_.find(coin);
        return it == positions_.end() ? 0.0 : it->second;
    }

    std::size_t activeOrderCount() const { return active_orders_.size(); }
    std::size_t submitCount() const { return submit_count_; }

    bool cancelRequested(OrderID orderId) const
    {
        const auto it = active_orders_.find(orderId);
        return it != active_orders_.end() && it->second.cancel_requested;
    }

    std::size_t cancelRequestCount(OrderID orderId) const
    {
        const auto it = cancel_request_count_.find(orderId);
        return it == cancel_request_count_.end() ? 0 : it->second;
    }
};
