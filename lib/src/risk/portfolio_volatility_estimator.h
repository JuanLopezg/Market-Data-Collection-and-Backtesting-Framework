#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "covariance_matrix.h"
#include "portfolio_weights.h"


/**************************************************************************************
 * Type    : PortfolioVolatilityEstimator
 * Purpose : Calculates annualized portfolio volatility from weights and covariance
 *
 * PortfolioWeights are dimensionless fractions of portfolio capital, so no monetary
 * capital is needed here. The returned volatility is a decimal annualized value, e.g.
 * 0.20 means 20%.
 **************************************************************************************/
class PortfolioVolatilityEstimator {
public:
    double estimate(
        const PortfolioWeights& portfolioWeights,
        const CovarianceMatrix& covariance
    ) const
    {
        if (portfolioWeights.size() == 0)
            return 0.0;

        double variance = 0.0;

        for (const auto& [leftCoin, leftWeight] : portfolioWeights.values()) {
            if (!covariance.contains(leftCoin))
                throw std::invalid_argument("Portfolio coin missing from covariance matrix");

            for (const auto& [rightCoin, rightWeight] : portfolioWeights.values()) {
                if (!covariance.contains(rightCoin))
                    throw std::invalid_argument("Portfolio coin missing from covariance matrix");

                variance += leftWeight * covariance.get(leftCoin, rightCoin) * rightWeight;
            }
        }

        // A valid covariance matrix should not produce negative variance. Allow a tiny
        // floating-point error around zero, but reject materially invalid results.
        if (variance < -1e-12)
            throw std::runtime_error("Portfolio variance is negative");

        return std::sqrt(std::max(0.0, variance));
    }
};
