#pragma once

#include <cmath>
#include <optional>
#include <stdexcept>

#include "portfolio_sizer.h"


/**************************************************************************************
 * Type    : EqualWeightSizer
 * Purpose : Assigns the same configured weight to every full-strength active signal
 *
 * Signal intensity scales the entry weight linearly:
 *   +1.0 -> +weightPerFullSignal
 *   +0.5 -> +0.5 * weightPerFullSignal
 *   -1.0 -> -weightPerFullSignal
 *
 * This class only calculates desired weights. Whether an already-open position should
 * actually be resized is decided later by the strategy's RebalancePolicy.
 **************************************************************************************/
class EqualWeightSizer final : public PortfolioSizer {
private:
    double weightPerFullSignal_ = 0.0;

public:
    explicit EqualWeightSizer(double weightPerFullSignal)
        : weightPerFullSignal_(weightPerFullSignal)
    {
        if (!std::isfinite(weightPerFullSignal_) || weightPerFullSignal_ < 0.0)
            throw std::invalid_argument("Equal weight must be finite and non-negative");
    }

    PortfolioSizerKind kind() const override
    {
        return PortfolioSizerKind::EqualWeight;
    }

    std::optional<TargetWeights> size(
        const SignalState& signals,
        const MarketData& marketData,
        Timestamp timestamp
    ) const override
    {
        (void)marketData;
        (void)timestamp;

        TargetWeights targetWeights;
        for (const auto& [coin, signal] : signals.values())
            targetWeights.set(coin, signal * weightPerFullSignal_);

        return targetWeights;
    }

    double weightPerFullSignal() const
    {
        return weightPerFullSignal_;
    }
};
