#pragma once

#include <cmath>
#include <stdexcept>
#include <utility>
#include "data_types.h"
#include <algorithm>
#include <string>

// Order commands, tracked lifecycle state and venue updates belong together. Fills remain separate economic events.

// Direction of an executable order
enum class OrderSide {
    Buy,
    Sell
};


// Immutable execution command submitted to an Exchange
//
// Lifecycle state deliberately does not live here. OrderManager wraps this command in an
// Order and tracks Submitted/Accepted/Partial/Filled/Canceled/Rejected independently.
// This keeps strategy intent serializable and separate from exchange state.
struct ExecutionOrder {
    OrderID order_id = 0;
    StrategyID strategy_id = 0;

    Timestamp created_at = 0;
    Timestamp active_from = 0;

    Coin coin;
    OrderSide side = OrderSide::Buy;
    double quantity = 0.0;
    // Long entry quantity is capped at the trigger and shrinks on an upward gap.
    // Short entry quantity is sized at min(trigger, execution open) by the planner.
    double entry_stop_price = 0.0;
    // Research short-entry bracket. The child covers only the actual filled units.
    double protective_stop_price = 0.0;
    OrderID parent_order_id = 0;

    ExecutionOrder() = default;

    ExecutionOrder(
        OrderID orderId,
        StrategyID strategyId,
        Timestamp createdAt,
        Timestamp activeFrom,
        Coin asset,
        OrderSide orderSide,
        double orderQuantity
    )
        : order_id(orderId),
          strategy_id(strategyId),
          created_at(createdAt),
          active_from(activeFrom),
          coin(std::move(asset)),
          side(orderSide),
          quantity(orderQuantity)
    {
        if (coin.empty())
            throw std::invalid_argument("Order asset cannot be empty");
        if (!std::isfinite(quantity) || quantity <= 0.0)
            throw std::invalid_argument("Order quantity must be finite and positive");
    }

    double signedQuantity() const
    {
        return side == OrderSide::Buy ? quantity : -quantity;
    }

    void validateConditional() const
    {
        if ((entry_stop_price != 0.0 || protective_stop_price != 0.0 || parent_order_id != 0) &&
            (!std::isfinite(quantity) || quantity <= 0.0))
            throw std::invalid_argument("Conditional quantity must be finite and positive");
        if (!std::isfinite(entry_stop_price) || entry_stop_price < 0.0 ||
            !std::isfinite(protective_stop_price) || protective_stop_price < 0.0)
            throw std::invalid_argument("Conditional prices must be finite and non-negative");
        if (entry_stop_price != 0.0 && active_from <= created_at)
            throw std::invalid_argument("Stop entry must activate after its decision close");
        if (parent_order_id != 0 && (side != OrderSide::Buy || entry_stop_price != 0.0 ||
            protective_stop_price == 0.0 || active_from < created_at))
            throw std::invalid_argument("Invalid protective short cover");
        if (parent_order_id == 0 && protective_stop_price != 0.0 &&
            (side != OrderSide::Sell || entry_stop_price == 0.0))
            throw std::invalid_argument("Protective stop must attach to a short stop entry");
    }
};

// Generic lifecycle state tracked for a new execution-layer order
//
// The explicit name avoids colliding with the legacy backtest OrderStatus still kept in
// data_types.h for old strategies/reporting compatibility.
enum class ExecutionOrderStatus {
    Created,
    Submitted,
    Accepted,
    PartiallyFilled,
    Filled,
    Canceled,
    Rejected
};


inline bool isTerminalExecutionOrderStatus(ExecutionOrderStatus status)
{
    return status == ExecutionOrderStatus::Filled ||
           status == ExecutionOrderStatus::Canceled ||
           status == ExecutionOrderStatus::Rejected;
}

// Mutable lifecycle state for one immutable ExecutionOrder command
struct TrackedOrder {
    ExecutionOrder request;
    ExecutionOrderStatus status = ExecutionOrderStatus::Created;

    double filled_quantity = 0.0;
    Timestamp updated_at = 0;
    bool cancel_requested = false;

    std::string exchange_order_id;
    std::string last_message;

    TrackedOrder() = default;

    explicit TrackedOrder(ExecutionOrder executionOrder)
        : request(std::move(executionOrder)),
          updated_at(request.created_at)
    {}

    double remainingQuantity() const
    {
        return std::max(0.0, request.quantity - filled_quantity);
    }

    double pendingSignedQuantity() const
    {
        if (isTerminalExecutionOrderStatus(status) || request.parent_order_id != 0)
            return 0.0;

        const double remaining = remainingQuantity();
        return request.side == OrderSide::Buy ? remaining : -remaining;
    }

    bool isOpen() const
    {
        return !isTerminalExecutionOrderStatus(status);
    }
};

// Exchange-originated lifecycle update, separate from actual Fill events
//
// exchange_order_id is intentionally a string because real exchanges do not share one
// identifier format. message may contain a reject/cancel reason for diagnostics.
struct OrderUpdate {
    OrderID order_id = 0;
    Timestamp timestamp = 0;
    ExecutionOrderStatus status = ExecutionOrderStatus::Created;
    std::string exchange_order_id;
    std::string message;

    OrderUpdate() = default;

    OrderUpdate(
        OrderID orderId,
        Timestamp updateTimestamp,
        ExecutionOrderStatus orderStatus,
        std::string exchangeOrderId = {},
        std::string updateMessage = {}
    )
        : order_id(orderId),
          timestamp(updateTimestamp),
          status(orderStatus),
          exchange_order_id(std::move(exchangeOrderId)),
          message(std::move(updateMessage))
    {
        if (order_id == 0)
            throw std::invalid_argument("Order update id must be non-zero");
    }
};
