#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>

#include "data_types.h"


/**************************************************************************************
 * Type    : PriceSnapshot
 * Purpose : Generic point-in-time prices by asset
 *
 * This type is shared by backtest and live infrastructure. It does not imply that the
 * stored price is a fill price; callers decide whether it represents an open, close,
 * quote, mark or another valid reference price.
 **************************************************************************************/
class PriceSnapshot {
private:
    std::unordered_map<Coin, double> prices_;

public:
    double get(const Coin& coin) const
    {
        const auto it = prices_.find(coin);
        if (it == prices_.end())
            throw std::out_of_range("Price not available for asset");

        return it->second;
    }

    void set(const Coin& coin, double price)
    {
        if (!std::isfinite(price) || price <= 0.0)
            throw std::invalid_argument("Price must be finite and positive");

        prices_[coin] = price;
    }

    bool contains(const Coin& coin) const
    {
        return prices_.find(coin) != prices_.end();
    }

    std::size_t size() const
    {
        return prices_.size();
    }

    const std::unordered_map<Coin, double>& values() const
    {
        return prices_;
    }
};
