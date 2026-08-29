#pragma once

#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <unordered_map>

#include "data_types.h"
#include "rebalance_decision.h"


/**************************************************************************************
 * Type    : RebalancePlan
 * Purpose : Strategy-level sizing/rebalance decisions produced for one timestamp
 *
 * referenceCapital is the strategy capital snapshot used when target weights were
 * calculated. It is intentionally stored with the plan so execution can later convert:
 *
 *   target weight * reference capital -> target monetary exposure -> quantity
 *
 * using the configured execution/fill price, not the strategy calculation close.
 * HOLD decisions are normally omitted from the map; missing coin therefore means HOLD.
 **************************************************************************************/
class RebalancePlan {
private:
    Timestamp timestamp_ = 0;
    double reference_capital_ = 0.0;
    std::unordered_map<Coin, RebalanceDecision> decisions_;

public:
    RebalancePlan(Timestamp timestamp, double referenceCapital)
        : timestamp_(timestamp), reference_capital_(referenceCapital)
    {
        if (!std::isfinite(reference_capital_) || reference_capital_ < 0.0)
            throw std::invalid_argument("Rebalance reference capital must be finite and non-negative");
    }

    void set(const Coin& coin, const RebalanceDecision& decision)
    {
        if (decision.action == RebalanceAction::Hold) {
            decisions_.erase(coin);
            return;
        }

        decisions_[coin] = decision;
    }

    RebalanceDecision get(const Coin& coin) const
    {
        const auto it = decisions_.find(coin);
        return it == decisions_.end() ? RebalanceDecision::hold() : it->second;
    }

    bool contains(const Coin& coin) const
    {
        return decisions_.find(coin) != decisions_.end();
    }

    std::size_t size() const
    {
        return decisions_.size();
    }

    const std::unordered_map<Coin, RebalanceDecision>& values() const
    {
        return decisions_;
    }

    Timestamp timestamp() const
    {
        return timestamp_;
    }

    double referenceCapital() const
    {
        return reference_capital_;
    }
};
