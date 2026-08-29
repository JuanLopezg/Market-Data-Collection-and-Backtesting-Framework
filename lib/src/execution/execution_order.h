#pragma once

#include <cmath>
#include <stdexcept>
#include <utility>

#include "data_types.h"


/**************************************************************************************
 * Type    : OrderSide
 * Purpose : Direction of an executable order
 **************************************************************************************/
enum class OrderSide {
    Buy,
    Sell
};


/**************************************************************************************
 * Type    : ExecutionOrder
 * Purpose : Immutable execution command submitted to an Exchange
 *
 * Lifecycle state deliberately does not live here. OrderManager wraps this command in an
 * Order and tracks Submitted/Accepted/Partial/Filled/Canceled/Rejected independently.
 * This keeps strategy intent serializable and separate from exchange state.
 **************************************************************************************/
struct ExecutionOrder {
    OrderID order_id = 0;
    StrategyID strategy_id = 0;

    Timestamp created_at = 0;
    Timestamp active_from = 0;

    Coin coin;
    OrderSide side = OrderSide::Buy;
    double quantity = 0.0;

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
};
