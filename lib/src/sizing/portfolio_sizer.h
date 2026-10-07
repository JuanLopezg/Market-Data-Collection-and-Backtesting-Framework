#pragma once

#include <cstddef>
#include <optional>

#include "data_types.h"
#include "signal_state.h"
#include "portfolio_weights.h"


// Optional dimensionless diagnostics produced by a PortfolioSizer
//
// Volatility values are annualized decimals: 0.20 means 20% annualized volatility.
// Weight values are fractions of strategy capital: 0.20 means 20% exposure.
struct PortfolioSizingDiagnostics {
    std::size_t active_assets = 0;

    double target_volatility = 0.0;
    double raw_signal_volatility = 0.0;
    double scaling_factor = 0.0;
    double pre_constraint_volatility = 0.0;
    double post_constraint_volatility = 0.0;

    double gross_before_constraints = 0.0;
    double gross_after_constraints = 0.0;
    double max_asset_weight_after_constraints = 0.0;

    double max_gross_leverage_limit = 0.0;
    double max_asset_weight_limit = 0.0;
    bool asset_cap_binding = false;
    bool gross_cap_binding = false;

    std::size_t rebalance_actions = 0;
};

// Stable identifier used by backtest validation/reporting code
//
// Keep this explicit instead of relying on RTTI/dynamic_cast. A future sizing method
// should add a new value here and define its own reference-comparison policy.
enum class PortfolioSizerKind {
    Generic,
    EqualWeight,
    VolatilityTarget
};

inline const char* portfolioSizerKindName(PortfolioSizerKind kind)
{
    switch (kind) {
        case PortfolioSizerKind::EqualWeight: return "EqualWeight";
        case PortfolioSizerKind::VolatilityTarget: return "VolatilityTarget";
        default: return "Generic";
    }
}


// Converts one strategy's current signals into desired target weights
//
// Different StrategyInstance objects may use different sizing methods. For example,
// one strategy can use volatility targeting while another uses equal/fixed weights.
// All sizing methods converge to the same TargetWeights representation.
class PortfolioSizer {
public:
    virtual ~PortfolioSizer() = default;

    // Identify the sizing family for diagnostics/reference validation
    virtual PortfolioSizerKind kind() const
    {
        return PortfolioSizerKind::Generic;
    }

    // Calculate desired weights for this strategy at one timestamp
    // Returns : std::nullopt when the sizing method cannot produce a valid estimate yet
    virtual std::optional<TargetWeights> size(
        const SignalState& signals,
        const MarketData& marketData,
        Timestamp timestamp
    ) const = 0;

    // Optional sizing diagnostics for validation/backtest analytics
    // Note    : Normal sizers may return std::nullopt. Volatility sizers can expose the
    // ex-ante volatility before and after strategy risk constraints.
    virtual std::optional<PortfolioSizingDiagnostics> diagnostics(
        const SignalState& signals,
        const TargetWeights& sizedWeights,
        const TargetWeights& constrainedWeights,
        const MarketData& marketData,
        Timestamp timestamp
    ) const
    {
        (void)signals;
        (void)sizedWeights;
        (void)constrainedWeights;
        (void)marketData;
        (void)timestamp;
        return std::nullopt;
    }
};
