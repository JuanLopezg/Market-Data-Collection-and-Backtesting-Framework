#pragma once

#include <cmath>
#include <stdexcept>

#include "execution_order.h"


/**************************************************************************************
 * Type    : ExecutionRiskGuard
 * Purpose : Final execution-level invariant check before an order is submitted
 *
 * Portfolio concentration/leverage limits belong to RiskConstraints and are already
 * reflected in targetQuantity. This guard does not optimize or resize the portfolio.
 * It only guarantees that a new order moves effective filled+pending quantity toward
 * the already-approved target without crossing/overshooting it.
 **************************************************************************************/
class ExecutionRiskGuard {
private:
    double quantity_epsilon_ = 1e-12;

public:
    explicit ExecutionRiskGuard(double quantityEpsilon = 1e-12)
        : quantity_epsilon_(quantityEpsilon)
    {
        if (!std::isfinite(quantity_epsilon_) || quantity_epsilon_ < 0.0)
            throw std::invalid_argument("Quantity epsilon must be finite and non-negative");
    }

    bool allows(
        const ExecutionOrder& order,
        double targetQuantity,
        double currentQuantity,
        double pendingSignedQuantity
    ) const
    {
        if (!std::isfinite(targetQuantity) ||
            !std::isfinite(currentQuantity) ||
            !std::isfinite(pendingSignedQuantity))
            return false;

        const double effectiveQuantity = currentQuantity + pendingSignedQuantity;
        const double requiredDelta = targetQuantity - effectiveQuantity;
        const double proposedDelta = order.signedQuantity();

        if (std::abs(requiredDelta) <= quantity_epsilon_)
            return false; // Already covered by fills + pending orders.

        if (requiredDelta > 0.0 && proposedDelta <= 0.0)
            return false;
        if (requiredDelta < 0.0 && proposedDelta >= 0.0)
            return false;

        // Never cross the approved target. Smaller orders remain valid for venues that
        // impose quantity/notional limits; another execution cycle can submit the rest.
        return std::abs(proposedDelta) <= std::abs(requiredDelta) + quantity_epsilon_;
    }

    void validate(
        const ExecutionOrder& order,
        double targetQuantity,
        double currentQuantity,
        double pendingSignedQuantity
    ) const
    {
        if (!allows(order, targetQuantity, currentQuantity, pendingSignedQuantity))
            throw std::logic_error("Execution order would duplicate or overshoot approved target");
    }
};
