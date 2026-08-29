#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>

#include "data_types.h"


using PositionQuantity = double;


/**************************************************************************************
 * Type    : PositionState
 * Purpose : Generic quantity state by asset
 *
 * This representation is intentionally independent of strategy, backtest and live
 * execution. It can be used for actual account positions, strategy attribution or
 * desired account targets.
 *
 * Missing assets are treated as quantity 0.
 **************************************************************************************/
class PositionState {
private:
    std::unordered_map<Coin, PositionQuantity> quantities_;

public:
    PositionQuantity get(const Coin& coin) const
    {
        const auto it = quantities_.find(coin);
        return it == quantities_.end() ? 0.0 : it->second;
    }

    void set(const Coin& coin, PositionQuantity quantity)
    {
        if (!std::isfinite(quantity))
            throw std::invalid_argument("Position quantity must be finite");

        if (quantity == 0.0) {
            quantities_.erase(coin);
            return;
        }

        quantities_[coin] = quantity;
    }

    void add(const Coin& coin, PositionQuantity quantityChange)
    {
        if (!std::isfinite(quantityChange))
            throw std::invalid_argument("Position quantity change must be finite");

        set(coin, get(coin) + quantityChange);
    }

    bool contains(const Coin& coin) const
    {
        return quantities_.find(coin) != quantities_.end();
    }

    std::size_t size() const
    {
        return quantities_.size();
    }

    const std::unordered_map<Coin, PositionQuantity>& values() const
    {
        return quantities_;
    }

    void clear()
    {
        quantities_.clear();
    }
};
