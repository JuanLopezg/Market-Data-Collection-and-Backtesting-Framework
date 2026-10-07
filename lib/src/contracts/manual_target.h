#pragma once

#include <cmath>
#include <map>
#include <string>

#include "data_types.h"

namespace ManualControl {

inline constexpr const char* kContractVersion = "step57-v1";
inline constexpr StrategyID kManualStrategyId = 900001U;

// Complete long-only manual allocation: asset weights plus cash must sum to one.
// The execution timestamp follows the decision timestamp; valid() checks shape
// and timing, while confirmation and request-identity handling belong to callers.
struct ManualTargetIntent {
    std::string request_id;
    std::string correlation_id;
    std::string actor;
    std::string request_hash;
    Timestamp decision_timestamp = 0;
    Timestamp execution_timestamp = 0;
    std::map<Coin, double> asset_weights;
    double cash_weight = 0.0; // Unallocated capital; CASH is not an executable asset symbol.

    bool valid(double tolerance = 1e-9) const
    {
        if (request_id.empty() || correlation_id.empty() || actor.empty() ||
            request_hash.empty() || decision_timestamp == 0 ||
            execution_timestamp <= decision_timestamp ||
            !std::isfinite(cash_weight) || cash_weight < 0.0 || cash_weight > 1.0)
            return false;

        double sum = cash_weight;
        for (const auto& [asset, weight] : asset_weights) {
            if (asset.empty() || asset == "CASH" || !std::isfinite(weight) ||
                weight < 0.0 || weight > 1.0)
                return false;
            sum += weight;
        }
        return std::abs(sum - 1.0) <= tolerance;
    }
};

} // namespace ManualControl
