#pragma once

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "rebalance_policy.h"


// Resizes a held position only when the desired weight differs from its current weight by
// at least a fraction of that current weight (user rule, 2026-10-15).
//
// threshold is relative. Example: 0.20 with a position weighing 10% of strategy capital
// resizes only when the desired weight is <= 8% or >= 12%; at 5%, only <= 4% or >= 6%.
// Entries, exits and side reversals always trigger regardless of the threshold.
//
// The current weight is derived from the already-filled virtual quantity at the current
// price, as in ThresholdRebalancePolicy (which uses an absolute weight difference instead):
// currentWeight = currentQuantity * currentPrice / strategyCapital
//
// This policy does NOT convert the desired weight into quantity. That conversion belongs
// to execution so the configured same-close/next-open convention can determine price.
class RelativeThresholdRebalancePolicy final : public RebalancePolicy {
private:
    double threshold_ = 0.0;

public:
    explicit RelativeThresholdRebalancePolicy(double threshold)
        : threshold_(threshold)
    {
        if (!std::isfinite(threshold_) || threshold_ < 0.0)
            throw std::invalid_argument("Relative rebalance threshold must be finite and non-negative");
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
        const double drift = std::abs(desiredWeight - currentWeight);
        const double requiredDrift = threshold_ * std::abs(currentWeight);
        // Decimal equality can round just below the boundary after subtraction.
        const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() *
            std::max(std::abs(desiredWeight), std::abs(currentWeight));
        if (drift >= requiredDrift || requiredDrift - drift <= tolerance)
            return RebalanceDecision::targetWeight(desiredWeight);

        return RebalanceDecision::hold();
    }

    double threshold() const
    {
        return threshold_;
    }
};
