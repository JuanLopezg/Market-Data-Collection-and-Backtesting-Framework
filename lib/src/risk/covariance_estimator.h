#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>
#include "data_types.h"
#include <optional>

// Annualized return covariance and the interface for estimating it from available history.

// Stores an annualized return covariance matrix for a set of coins
//
// Values are covariances of asset returns, not monetary exposures. Coin ordering is
// kept internally so portfolio-volatility calculations can use the matrix consistently.
class CovarianceMatrix {
private:
    std::vector<Coin> coins_;
    std::unordered_map<Coin, std::size_t> indexes_;
    std::vector<std::vector<double>> values_;

public:
    CovarianceMatrix() = default;

    CovarianceMatrix(
        std::vector<Coin> coins,
        std::vector<std::vector<double>> values
    )
        : coins_(std::move(coins)), values_(std::move(values))
    {
        if (values_.size() != coins_.size())
            throw std::invalid_argument("Covariance matrix dimensions do not match coins");

        for (std::size_t i = 0; i < coins_.size(); ++i) {
            if (indexes_.find(coins_[i]) != indexes_.end())
                throw std::invalid_argument("Covariance matrix contains duplicate coins");

            indexes_[coins_[i]] = i;

            if (values_[i].size() != coins_.size())
                throw std::invalid_argument("Covariance matrix must be square");

            for (const double value : values_[i]) {
                if (!std::isfinite(value))
                    throw std::invalid_argument("Covariance matrix values must be finite");
            }
        }
    }

    // Return covariance between two coins
    double get(const Coin& left, const Coin& right) const
    {
        const auto leftIt = indexes_.find(left);
        const auto rightIt = indexes_.find(right);

        if (leftIt == indexes_.end() || rightIt == indexes_.end())
            throw std::out_of_range("Coin not present in covariance matrix");

        return values_[leftIt->second][rightIt->second];
    }

    bool contains(const Coin& coin) const
    {
        return indexes_.find(coin) != indexes_.end();
    }

    std::size_t size() const
    {
        return coins_.size();
    }

    const std::vector<Coin>& coins() const
    {
        return coins_;
    }
};

// Interface for estimating asset-return covariance at a point in time
//
// Implementations must use only information available at or before timestamp. Returning
// std::nullopt means that there is not enough valid history to produce an estimate.
class CovarianceEstimator {
public:
    virtual ~CovarianceEstimator() = default;

    virtual std::optional<CovarianceMatrix> estimate(
        const MarketData& marketData,
        Timestamp timestamp,
        const std::vector<Coin>& coins
    ) const = 0;
};
