#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>

#include "data_types.h"


using PortfolioWeight = double;


/**************************************************************************************
 * Type    : PortfolioWeights
 * Purpose : Stores dimensionless portfolio weights by coin
 *
 * A weight represents exposure as a fraction of the capital that will eventually be
 * assigned to the portfolio. For example, +0.10 means +10% exposure and -0.25 means
 * -25% exposure. Weights may exceed 1.0 in magnitude when leverage is allowed.
 *
 * No monetary units belong in this type. Portfolio construction, volatility targeting
 * and percentage-based risk constraints can therefore operate without knowing capital.
 * Coins with weight 0 are not stored internally; a missing coin therefore means 0.
 **************************************************************************************/
class PortfolioWeights {
private:
    std::unordered_map<Coin, PortfolioWeight> weights_;

public:
    /**************************************************************************************
     * Purpose : Return the current weight for a coin
     * Note    : Coins not present in the portfolio are treated as weight 0
     **************************************************************************************/
    PortfolioWeight get(const Coin& coin) const
    {
        const auto it = weights_.find(coin);
        return it == weights_.end() ? 0.0 : it->second;
    }

    /**************************************************************************************
     * Purpose : Set the weight for a coin
     * Note    : Weight must be finite. Setting 0 removes the coin from the internal map
     **************************************************************************************/
    void set(const Coin& coin, PortfolioWeight value)
    {
        if (!std::isfinite(value))
            throw std::invalid_argument("Portfolio weight must be finite");

        if (value == 0.0) {
            weights_.erase(coin);
            return;
        }

        weights_[coin] = value;
    }

    /**************************************************************************************
     * Purpose : Add a weight change to a coin
     * Note    : Useful when portfolio weights need to be combined before a risk step
     **************************************************************************************/
    void add(const Coin& coin, PortfolioWeight value)
    {
        if (!std::isfinite(value))
            throw std::invalid_argument("Portfolio weight must be finite");

        set(coin, get(coin) + value);
    }

    bool contains(const Coin& coin) const
    {
        return weights_.find(coin) != weights_.end();
    }

    std::size_t size() const
    {
        return weights_.size();
    }

    const std::unordered_map<Coin, PortfolioWeight>& values() const
    {
        return weights_;
    }

    void clear()
    {
        weights_.clear();
    }
};
