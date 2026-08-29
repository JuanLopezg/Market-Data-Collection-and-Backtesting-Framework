#pragma once

#include <cmath>
#include <stdexcept>

#include "target_portfolio.h"
#include "target_weights.h"


/**************************************************************************************
 * Type    : TargetPortfolioBuilder
 * Purpose : Converts final strategy target weights into monetary target exposures
 *
 * This is intentionally the last portfolio-construction step that needs strategy capital.
 * All signal processing, covariance, volatility targeting and percentage-based risk
 * constraints should happen before this conversion.
 *
 * In the current system referenceCapital is denominated in USD, so the resulting
 * TargetPortfolio values are USD exposures.
 **************************************************************************************/
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
