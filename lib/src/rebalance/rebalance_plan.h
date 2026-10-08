#pragma once

#include <cmath>
#include <stdexcept>
#include <cstddef>
#include <unordered_map>
#include "data_types.h"

// Rebalance actions and the timestamp/capital snapshot used to resolve their eventual execution quantity.

// Describes what a strategy wants to do with one asset target
//
// Hold:
// Keep the already-filled virtual quantity unchanged.
//
// Flat:
// Fully close the strategy's virtual position in this asset.
//
// TargetWeight:
// Resize/open the strategy to the supplied weight. Conversion to an executable
// quantity happens later, using the configured execution timing and fill price.
enum class RebalanceAction {
    Hold,
    Flat,
    TargetWeight
};


struct RebalanceDecision {
    RebalanceAction action = RebalanceAction::Hold;
    double target_weight = 0.0;
    // Optional one-following-bar stop entry. Zero keeps market execution.
    double entry_stop_price = 0.0;
    double protective_stop_price = 0.0;

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

    static RebalanceDecision stopEntry(double weight, double triggerPrice)
    {
        if (!std::isfinite(weight) || weight == 0.0 ||
            !std::isfinite(triggerPrice) || triggerPrice <= 0.0)
            throw std::invalid_argument("Stop entry requires nonzero weight and positive trigger price");
        return {RebalanceAction::TargetWeight, weight, triggerPrice};
    }
};

// Strategy-level sizing/rebalance decisions produced for one timestamp
//
// referenceCapital is the strategy capital snapshot used when target weights were
// calculated. It is intentionally stored with the plan so execution can later convert:
//
// target weight * reference capital -> target monetary exposure -> quantity
//
// using the configured execution/fill price, not the strategy calculation close.
// HOLD decisions are normally omitted from the map; missing coin therefore means HOLD.
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
