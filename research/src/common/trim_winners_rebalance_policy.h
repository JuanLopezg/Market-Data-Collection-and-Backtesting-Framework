#pragma once

#include <cmath>
#include <limits>
#include <stdexcept>

#include "rebalance_policy.h"


// Research-only variant of RelativeThresholdRebalancePolicy that resizes winners only
// (user study, 2026-10-21): a held position that grew above its desired weight by at
// least threshold x its current weight is trimmed back to the desired weight; a position
// that fell below its desired weight is never topped up. Entries, exits and side
// reversals always happen.
//
// A lower desired weight (for example a smaller signal strength) also counts as a
// reduction and is applied; a higher desired weight is only applied on entry.
class TrimWinnersRebalancePolicy final : public RebalancePolicy {
private:
    double threshold_ = 0.0;

public:
    explicit TrimWinnersRebalancePolicy(double threshold)
        : threshold_(threshold)
    {
        if (!std::isfinite(threshold_) || threshold_ < 0.0)
            throw std::invalid_argument("Trim-winners rebalance threshold must be finite and non-negative");
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

        if (currentPrice <= 0.0 || strategyCapital <= 0.0)
            throw std::invalid_argument("Price and strategy capital must be positive for rebalancing");

        const double currentWeight = currentQuantity * currentPrice / strategyCapital;
        // Only shrink the absolute exposure: never add to a position that lost weight.
        if (std::abs(desiredWeight) >= std::abs(currentWeight))
            return RebalanceDecision::hold();

        const double drift = std::abs(currentWeight) - std::abs(desiredWeight);
        const double requiredDrift = threshold_ * std::abs(currentWeight);
        const double tolerance = 8.0 * std::numeric_limits<double>::epsilon() *
            std::max(std::abs(desiredWeight), std::abs(currentWeight));
        if (drift >= requiredDrift || requiredDrift - drift <= tolerance)
            return RebalanceDecision::targetWeight(desiredWeight);

        return RebalanceDecision::hold();
    }
};
