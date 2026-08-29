#pragma once

#include <stdexcept>

#include "execution_reference_prices.h"
#include "rebalance_plan.h"
#include "target_portfolio.h"
#include "virtual_position_state.h"


/**************************************************************************************
 * Type    : StrategyTargetResolver
 * Purpose : Resolve one strategy's rebalance decisions into a monetary target portfolio
 *
 * Rules:
 *   HOLD:
 *     Preserve the already-filled virtual quantity. Its monetary exposure is simply
 *     quantity * current execution reference price.
 *
 *   FLAT:
 *     Desired monetary exposure becomes 0.
 *
 *   TARGET_WEIGHT:
 *     Desired monetary exposure becomes targetWeight * plan.referenceCapital().
 *
 * This does not create orders and does not mutate VirtualPositionState.
 **************************************************************************************/
class StrategyTargetResolver {
public:
    TargetPortfolio resolve(
        const RebalancePlan& plan,
        const VirtualPositionState& currentPositions,
        const ExecutionReferencePrices& prices
    ) const
    {
        TargetPortfolio target;

        // Start from current filled quantities so missing decisions naturally mean HOLD.
        for (const auto& [coin, quantity] : currentPositions.values()) {
            if (!prices.contains(coin))
                throw std::runtime_error("Missing execution reference price for held asset");

            target.set(coin, quantity * prices.get(coin));
        }

        // Explicit decisions override the held exposure.
        for (const auto& [coin, decision] : plan.values()) {
            switch (decision.action) {
                case RebalanceAction::Hold:
                    break;

                case RebalanceAction::Flat:
                    target.set(coin, 0.0);
                    break;

                case RebalanceAction::TargetWeight:
                    target.set(coin, decision.target_weight * plan.referenceCapital());
                    break;
            }
        }

        return target;
    }
};
