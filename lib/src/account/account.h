#pragma once

#include <cmath>
#include <stdexcept>
#include <unordered_map>
#include <utility>

#include "fill.h"
#include "position_state.h"
#include "price_snapshot.h"


/**************************************************************************************
 * Type    : Account
 * Purpose : Actual cash and filled positions owned by the account
 *
 * This is reality, not strategy intent. Account changes only from Fill objects.
 * Equity is marked from an externally supplied PriceSnapshot so the same object can be
 * used in historical backtests and live trading.
 **************************************************************************************/
class Account {
private:
    double cash_ = 0.0;
    PositionState positions_;

public:
    explicit Account(double initialCash)
        : cash_(initialCash)
    {
        if (!std::isfinite(cash_) || cash_ <= 0.0)
            throw std::invalid_argument("Initial account cash must be finite and positive");
    }

    void applyFill(const Fill& fill)
    {
        fill.validate();

        const double signedQuantity = fill.signedQuantity();
        cash_ -= signedQuantity * fill.price;
        cash_ -= fill.commission;
        positions_.add(fill.coin, signedQuantity);
    }

    /**************************************************************************************
     * Purpose : Restore authoritative filled account state during startup recovery
     **************************************************************************************/
    void restoreState(double cash, const std::unordered_map<Coin, double>& positions)
    {
        if (!std::isfinite(cash))
            throw std::invalid_argument("Restored account cash must be finite");

        PositionState restored;
        for (const auto& [coin, quantity] : positions)
            restored.set(coin, quantity);

        cash_ = cash;
        positions_ = std::move(restored);
    }

    double equity(const PriceSnapshot& prices) const
    {
        double result = cash_;

        for (const auto& [coin, quantity] : positions_.values()) {
            if (!prices.contains(coin))
                throw std::runtime_error("Missing mark price for account position");

            result += quantity * prices.get(coin);
        }

        return result;
    }

    double cash() const
    {
        return cash_;
    }

    const PositionState& positions() const
    {
        return positions_;
    }

    PositionState& positions()
    {
        return positions_;
    }
};
