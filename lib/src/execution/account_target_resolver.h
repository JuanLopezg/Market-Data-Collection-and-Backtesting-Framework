#pragma once

#include <stdexcept>

#include "execution_reference_prices.h"
#include "target_portfolio.h"
#include "target_position_state.h"


/**************************************************************************************
 * Type    : AccountTargetResolver
 * Purpose : Convert the aggregated monetary account target into desired asset quantities
 *           immediately before order planning
 *
 * The reference price is only used to choose an order quantity. The final filled
 * quantity/value remains whatever the exchange actually executes.
 **************************************************************************************/
class AccountTargetResolver {
public:
    TargetPositionState resolve(
        const TargetPortfolio& accountTarget,
        const ExecutionReferencePrices& prices
    ) const
    {
        TargetPositionState targetPositions;

        for (const auto& [coin, exposure] : accountTarget.values()) {
            if (!prices.contains(coin))
                throw std::runtime_error("Missing execution reference price for target asset");

            targetPositions.set(coin, exposure / prices.get(coin));
        }

        return targetPositions;
    }
};
