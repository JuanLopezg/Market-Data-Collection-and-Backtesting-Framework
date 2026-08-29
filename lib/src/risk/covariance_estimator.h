#pragma once

#include <optional>
#include <vector>

#include "covariance_matrix.h"


/**************************************************************************************
 * Type    : CovarianceEstimator
 * Purpose : Interface for estimating asset-return covariance at a point in time
 *
 * Implementations must use only information available at or before timestamp. Returning
 * std::nullopt means that there is not enough valid history to produce an estimate.
 **************************************************************************************/
class CovarianceEstimator {
public:
    virtual ~CovarianceEstimator() = default;

    virtual std::optional<CovarianceMatrix> estimate(
        const MarketData& marketData,
        Timestamp timestamp,
        const std::vector<Coin>& coins
    ) const = 0;
};
