#pragma once

#include <algorithm>
#include <cmath>
#include <stdexcept>

#include "portfolio_weights.h"


// Applies hard percentage-based limits to strategy target weights
//
// maxGrossLeverage:
// Maximum sum of absolute target weights. Example: 1.50 means 150% gross exposure.
//
// maxAssetWeight:
// Maximum absolute weight for any single asset. Example: 0.20 means 20%.
//
// Both limits are dimensionless and therefore apply before strategy capital is introduced.
// A value of 0 disables all exposure through that limit.
struct RiskConstraintEvaluation {
    TargetWeights asset_capped_weights;
    double gross_after_asset_cap = 0.0;
    double gross_scale = 1.0;
};

class RiskConstraints {
private:
    double maxGrossLeverage_ = 0.0;
    double maxAssetWeight_ = 0.0;

public:
    RiskConstraints(double maxGrossLeverage, double maxAssetWeight)
        : maxGrossLeverage_(maxGrossLeverage), maxAssetWeight_(maxAssetWeight)
    {
        if (!std::isfinite(maxGrossLeverage_) || maxGrossLeverage_ < 0.0)
            throw std::invalid_argument("Maximum gross leverage must be finite and non-negative");

        if (!std::isfinite(maxAssetWeight_) || maxAssetWeight_ < 0.0)
            throw std::invalid_argument("Maximum asset weight must be finite and non-negative");
    }

    TargetWeights apply(const TargetWeights& desiredWeights, RiskConstraintEvaluation* evaluation = nullptr) const
    {
        TargetWeights constrained;

        // Apply the per-asset hard cap first.
        for (const auto& [coin, weight] : desiredWeights.values()) {
            const double capped = std::clamp(weight, -maxAssetWeight_, maxAssetWeight_);
            constrained.set(coin, capped);
        }

        double gross = 0.0;
        for (const auto& [coin, weight] : constrained.values()) {
            (void)coin;
            gross += std::abs(weight);
        }
        if (evaluation) {
            evaluation->asset_capped_weights = constrained;
            evaluation->gross_after_asset_cap = gross;
            evaluation->gross_scale = 1.0;
        }

        // If gross leverage is still too high, preserve relative proportions and scale down.
        if (gross > maxGrossLeverage_ && gross > 0.0) {
            const double scale = maxGrossLeverage_ / gross;
            if (evaluation)
                evaluation->gross_scale = scale;
            TargetWeights scaled;

            for (const auto& [coin, weight] : constrained.values())
                scaled.set(coin, weight * scale);

            return scaled;
        }

        return constrained;
    }

    double maxGrossLeverage() const
    {
        return maxGrossLeverage_;
    }

    double maxAssetWeight() const
    {
        return maxAssetWeight_;
    }
};
