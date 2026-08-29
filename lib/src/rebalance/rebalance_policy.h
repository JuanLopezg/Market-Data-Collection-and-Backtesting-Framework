#pragma once

#include "rebalance_decision.h"
#include "signal_state.h"


/**************************************************************************************
 * Type    : RebalancePolicy
 * Purpose : Decides whether a calculated desired weight should actually change a holding
 *
 * Sizing answers "what weight would I like?". Rebalancing answers "should I modify the
 * quantity I already hold now?". Keeping these separate allows equal-weight strategies
 * to size only on entry while volatility-target strategies can resize over time.
 **************************************************************************************/
class RebalancePolicy {
public:
    virtual ~RebalancePolicy() = default;

    virtual RebalanceDecision decide(
        SignalValue signal,
        double desiredWeight,
        double currentQuantity,
        double currentPrice,
        double strategyCapital
    ) const = 0;
};
