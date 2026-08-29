#pragma once

#include <cmath>
#include <optional>
#include <stdexcept>

#include "portfolio_weights.h"


/**************************************************************************************
 * Type    : VolatilityTarget
 * Purpose : Scales base portfolio weights to a desired annualized volatility
 *
 * Volatility values are decimals: 0.20 means 20% annualized volatility. The class only
 * changes the magnitude of weights; relative long/short proportions are preserved.
 * No monetary capital is needed or introduced here.
 *
 * Hard limits such as maximum gross leverage or maximum asset weight are intentionally
 * not handled here. Those constraints belong to the risk layer after this scaling step.
 **************************************************************************************/
class VolatilityTarget {
private:
    double targetVolatility_ = 0.0;

public:
    explicit VolatilityTarget(double targetVolatility)
        : targetVolatility_(targetVolatility)
    {
        if (!std::isfinite(targetVolatility_) || targetVolatility_ < 0.0)
            throw std::invalid_argument("Target volatility must be finite and non-negative");
    }

    /**************************************************************************************
     * Purpose : Scale base weights to the configured volatility target
     * Returns : PortfolioWeights when a finite scaling factor can be calculated
     *
     * An empty base portfolio remains empty: volatility targeting must never create an
     * exposure when the strategy has no active signal.
     *
     * A non-empty portfolio with zero estimated volatility cannot be scaled to a positive
     * target using a finite multiplier, so std::nullopt is returned.
     **************************************************************************************/
    std::optional<PortfolioWeights> apply(
        const PortfolioWeights& baseWeights,
        double estimatedVolatility
    ) const
    {
        if (!std::isfinite(estimatedVolatility) || estimatedVolatility < 0.0)
            throw std::invalid_argument("Estimated volatility must be finite and non-negative");

        PortfolioWeights targetWeights;

        if (baseWeights.size() == 0 || targetVolatility_ == 0.0)
            return targetWeights;

        if (estimatedVolatility == 0.0)
            return std::nullopt;

        const double scale = targetVolatility_ / estimatedVolatility;

        if (!std::isfinite(scale))
            return std::nullopt;

        for (const auto& [coin, weight] : baseWeights.values()) {
            const double scaledWeight = weight * scale;

            if (!std::isfinite(scaledWeight))
                return std::nullopt;

            targetWeights.set(coin, scaledWeight);
        }

        return targetWeights;
    }

    double targetVolatility() const
    {
        return targetVolatility_;
    }
};
