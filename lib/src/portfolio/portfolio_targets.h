#pragma once

// Construct monetary targets and net them across strategies after sizing and risk.

#include <cmath>
#include <stdexcept>
#include <vector>

#include "portfolio.h"
#include "portfolio_weights.h"

// Converts final strategy target weights into monetary target exposures
//
// This is intentionally the last portfolio-construction step that needs strategy capital.
// All signal processing, covariance, volatility targeting and percentage-based risk
// constraints should happen before this conversion.
//
// In the current system referenceCapital is denominated in USD, so the resulting
// TargetPortfolio values are USD exposures.
class TargetPortfolioBuilder {
public:
    TargetPortfolio build(
        const TargetWeights& targetWeights,
        double referenceCapital
    ) const
    {
        if (!std::isfinite(referenceCapital) || referenceCapital < 0.0)
            throw std::invalid_argument("Reference capital must be finite and non-negative");

        TargetPortfolio targetPortfolio;

        for (const auto& [coin, weight] : targetWeights.values())
            targetPortfolio.set(coin, referenceCapital * weight);

        return targetPortfolio;
    }
};

// Net independently-sized strategy target portfolios asset by asset
//
// No additional risk layer is applied here. Each strategy has already completed its own
// sizing/risk/rebalance process before reaching this boundary.
class PortfolioAggregator {
public:
    TargetPortfolio aggregate(const std::vector<TargetPortfolio>& strategyTargets) const
    {
        TargetPortfolio result;

        for (const TargetPortfolio& target : strategyTargets) {
            for (const auto& [coin, exposure] : target.values())
                result.add(coin, exposure);
        }

        return result;
    }
};
