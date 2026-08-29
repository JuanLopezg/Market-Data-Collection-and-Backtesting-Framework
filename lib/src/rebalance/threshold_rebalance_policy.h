#pragma once

#include <cmath>
#include <stdexcept>

#include "rebalance_policy.h"


/**************************************************************************************
 * Type    : ThresholdRebalancePolicy
 * Purpose : Rebalances when current weight drifts far enough from desired weight
 *
 * threshold is an absolute weight difference. Example: 0.02 means two percentage points.
 * Entries, exits and side reversals always trigger regardless of the threshold.
 *
 * The current weight is derived from the already-filled virtual quantity:
 *   currentWeight = currentQuantity * currentPrice / strategyCapital
 *
 * This policy does NOT convert the desired weight into quantity. That conversion belongs
 * to execution so the configured same-close/next-open convention can determine price.
 **************************************************************************************/
class ThresholdRebalancePolicy final : public RebalancePolicy {
private:
    double threshold_ = 0.0;

public:
    explicit ThresholdRebalancePolicy(double threshold)
        : threshold_(threshold)
    {
        if (!std::isfinite(threshold_) || threshold_ < 0.0)
            throw std::invalid_argument("Rebalance threshold must be finite and non-negative");
    }

    RebalanceDecision decide(
        SignalValue signal,
        double desiredWeight,
        double currentQuantity,
        double currentPrice,
        double strategyCapital
    ) const override
    {
        if (!std::isfinite(signal) || signal < -1.0 || signal > 1.0)
            throw std::invalid_argument("Signal must be finite and inside [-1, +1]");
        if (!std::isfinite(desiredWeight) || !std::isfinite(currentQuantity) ||
            !std::isfinite(currentPrice) || !std::isfinite(strategyCapital))
            throw std::invalid_argument("Rebalance inputs must be finite");

        if (signal == 0.0 || desiredWeight == 0.0)
            return currentQuantity == 0.0 ? RebalanceDecision::hold() : RebalanceDecision::flat();

        if (currentQuantity == 0.0)
            return RebalanceDecision::targetWeight(desiredWeight);

        const bool sameSide = (currentQuantity > 0.0 && desiredWeight > 0.0) ||
                              (currentQuantity < 0.0 && desiredWeight < 0.0);
        if (!sameSide)
            return RebalanceDecision::targetWeight(desiredWeight);

        if (currentPrice <= 0.0)
            throw std::invalid_argument("Current price must be positive for threshold rebalancing");
        if (strategyCapital <= 0.0)
            throw std::invalid_argument("Strategy capital must be positive for threshold rebalancing");

        const double currentWeight = currentQuantity * currentPrice / strategyCapital;
        if (std::abs(desiredWeight - currentWeight) >= threshold_)
            return RebalanceDecision::targetWeight(desiredWeight);

        return RebalanceDecision::hold();
    }

    double threshold() const
    {
        return threshold_;
    }
};
