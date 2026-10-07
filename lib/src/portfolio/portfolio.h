#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include "data_types.h"

// Monetary exposure and its final target name. These values are not dimensionless weights.

using ExposureValue = double;


// Stores monetary exposure by coin after capital has been applied
//
// This type belongs at the monetary boundary of portfolio construction. Risk calculations
// should work with PortfolioWeights first; only final target weights are converted into
// Portfolio values using the strategy's reference capital.
//
// In the current system the account currency is USD, but no currency is encoded in the
// type itself. Coins with exposure 0 are not stored internally; a missing coin means 0.
class Portfolio {
private:
    std::unordered_map<Coin, ExposureValue> exposures_;

public:
    // Return the current exposure for a coin
    // Note    : Coins not present in the portfolio are treated as exposure 0
    ExposureValue get(const Coin& coin) const
    {
        const auto it = exposures_.find(coin);
        return it == exposures_.end() ? 0.0 : it->second;
    }

    // Set the exposure for a coin
    // Note    : Exposure must be finite. Setting 0 removes the coin from the internal map
    void set(const Coin& coin, ExposureValue value)
    {
        if (!std::isfinite(value))
            throw std::invalid_argument("Portfolio exposure must be finite");

        if (value == 0.0) {
            exposures_.erase(coin);
            return;
        }

        exposures_[coin] = value;
    }

    // Add an exposure change to a coin
    // Note    : Used when aggregating independently risk-adjusted strategy targets
    void add(const Coin& coin, ExposureValue value)
    {
        if (!std::isfinite(value))
            throw std::invalid_argument("Portfolio exposure must be finite");

        set(coin, get(coin) + value);
    }

    // Check whether a coin currently has non-zero exposure
    bool contains(const Coin& coin) const
    {
        return exposures_.find(coin) != exposures_.end();
    }

    // Number of coins that currently have non-zero exposure
    std::size_t size() const
    {
        return exposures_.size();
    }

    // Read all currently stored exposures
    // Note    : Exposes a const reference so callers cannot bypass set() validation
    const std::unordered_map<Coin, ExposureValue>& values() const
    {
        return exposures_;
    }

    // Remove all desired exposures
    void clear()
    {
        exposures_.clear();
    }
};

// Monetary target produced after final target weights receive strategy capital
//
// This is the object that can later be aggregated across independently sized strategies
// and passed to execution. In the current system its values are USD exposures, although
// the type itself remains currency-agnostic.
using TargetPortfolio = Portfolio;
