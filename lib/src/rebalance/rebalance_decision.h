#pragma once

#include <cmath>
#include <stdexcept>


/**************************************************************************************
 * Type    : RebalanceAction
 * Purpose : Describes what a strategy wants to do with one asset target
 *
 * Hold:
 *   Keep the already-filled virtual quantity unchanged.
 *
 * Flat:
 *   Fully close the strategy's virtual position in this asset.
 *
 * TargetWeight:
 *   Resize/open the strategy to the supplied weight. Conversion to an executable
 *   quantity happens later, using the configured execution timing and fill price.
 **************************************************************************************/
enum class RebalanceAction {
    Hold,
    Flat,
    TargetWeight
};


struct RebalanceDecision {
    RebalanceAction action = RebalanceAction::Hold;
    double target_weight = 0.0;

    static RebalanceDecision hold()
    {
        return {RebalanceAction::Hold, 0.0};
    }

    static RebalanceDecision flat()
    {
        return {RebalanceAction::Flat, 0.0};
    }

    static RebalanceDecision targetWeight(double weight)
    {
        if (!std::isfinite(weight))
            throw std::invalid_argument("Rebalance target weight must be finite");

        if (weight == 0.0)
            return flat();

        return {RebalanceAction::TargetWeight, weight};
    }
};
