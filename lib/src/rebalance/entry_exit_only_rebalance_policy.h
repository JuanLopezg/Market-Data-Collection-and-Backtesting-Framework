#pragma once

#include <cmath>
#include <stdexcept>

#include "rebalance_policy.h"


/**************************************************************************************
 * Type    : EntryExitOnlyRebalancePolicy
 * Purpose : Sizes a position when it first becomes active and otherwise keeps quantity
 *
 * Behaviour:
 *   0 signal + no position      -> HOLD
 *   0 signal + open position    -> FLAT
 *   active signal + no position -> TARGET_WEIGHT (new entry)
 *   active signal + same-side position -> HOLD
 *   signal changes side         -> TARGET_WEIGHT (close/reverse through execution later)
 *
 * Changes such as +0.5 -> +1.0 do not resize an already-open same-side position. This
 * is the intended initial behaviour for equal-weight strategies.
 **************************************************************************************/
class EntryExitOnlyRebalancePolicy final : public RebalancePolicy {
public:
    RebalanceDecision decide(
        SignalValue signal,
        double desiredWeight,
        double currentQuantity,
        double currentPrice,
        double strategyCapital
    ) const override
    {
        (void)currentPrice;
        (void)strategyCapital;

        if (!std::isfinite(signal) || signal < -1.0 || signal > 1.0)
            throw std::invalid_argument("Signal must be finite and inside [-1, +1]");
        if (!std::isfinite(desiredWeight) || !std::isfinite(currentQuantity))
            throw std::invalid_argument("Rebalance inputs must be finite");

        if (signal == 0.0 || desiredWeight == 0.0)
            return currentQuantity == 0.0 ? RebalanceDecision::hold() : RebalanceDecision::flat();

        if (currentQuantity == 0.0)
            return RebalanceDecision::targetWeight(desiredWeight);

        const bool sameSide = (currentQuantity > 0.0 && signal > 0.0) ||
                              (currentQuantity < 0.0 && signal < 0.0);

        return sameSide ? RebalanceDecision::hold()
                        : RebalanceDecision::targetWeight(desiredWeight);
    }
};
