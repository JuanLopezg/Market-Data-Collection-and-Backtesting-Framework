#pragma once

#include <cmath>
#include <stdexcept>
#include <utility>

#include "execution_order.h"


/**************************************************************************************
 * Type    : Fill
 * Purpose : Quantity that actually executed at the exchange
 *
 * Position/account state changes only from Fill objects. Creating a target or submitting
 * an order never implies that anything was executed.
 **************************************************************************************/
struct Fill {
    FillID fill_id = 0;
    OrderID order_id = 0;
    StrategyID strategy_id = 0;

    Timestamp timestamp = 0;
    Coin coin;
    OrderSide side = OrderSide::Buy;

    double quantity = 0.0;
    double price = 0.0;
    double commission = 0.0;

    double signedQuantity() const
    {
        return side == OrderSide::Buy ? quantity : -quantity;
    }

    void validate() const
    {
        if (coin.empty())
            throw std::invalid_argument("Fill asset cannot be empty");
        if (!std::isfinite(quantity) || quantity <= 0.0)
            throw std::invalid_argument("Fill quantity must be finite and positive");
        if (!std::isfinite(price) || price <= 0.0)
            throw std::invalid_argument("Fill price must be finite and positive");
        if (!std::isfinite(commission) || commission < 0.0)
            throw std::invalid_argument("Fill commission must be finite and non-negative");
    }
};
