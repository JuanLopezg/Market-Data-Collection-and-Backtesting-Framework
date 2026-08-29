#pragma once

#include <optional>

#include "data_types.h"
#include "portfolio_sizing_diagnostics.h"
#include "signal_state.h"
#include "target_weights.h"


/**************************************************************************************
 * Type    : PortfolioSizerKind
 * Purpose : Stable identifier used by backtest validation/reporting code
 *
 * Keep this explicit instead of relying on RTTI/dynamic_cast. A future sizing method
 * should add a new value here and define its own reference-comparison policy.
 **************************************************************************************/
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


/**************************************************************************************
 * Type    : PortfolioSizer
 * Purpose : Converts one strategy's current signals into desired target weights
 *
 * Different StrategyInstance objects may use different sizing methods. For example,
 * one strategy can use volatility targeting while another uses equal/fixed weights.
 * All sizing methods converge to the same TargetWeights representation.
 **************************************************************************************/
class PortfolioSizer {
public:
    virtual ~PortfolioSizer() = default;

    /**************************************************************************************
     * Purpose : Identify the sizing family for diagnostics/reference validation
     **************************************************************************************/
    virtual PortfolioSizerKind kind() const
    {
        return PortfolioSizerKind::Generic;
    }

    /**************************************************************************************
     * Purpose : Calculate desired weights for this strategy at one timestamp
     * Returns : std::nullopt when the sizing method cannot produce a valid estimate yet
     **************************************************************************************/
    virtual std::optional<TargetWeights> size(
        const SignalState& signals,
        const MarketData& marketData,
        Timestamp timestamp
    ) const = 0;

    /**************************************************************************************
     * Purpose : Optional sizing diagnostics for validation/backtest analytics
     * Note    : Normal sizers may return std::nullopt. Volatility sizers can expose the
     *           ex-ante volatility before and after strategy risk constraints.
     **************************************************************************************/
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
