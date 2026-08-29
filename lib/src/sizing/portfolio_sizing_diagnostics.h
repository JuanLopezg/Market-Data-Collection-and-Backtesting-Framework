#pragma once

#include <cstddef>


/**************************************************************************************
 * Type    : PortfolioSizingDiagnostics
 * Purpose : Optional dimensionless diagnostics produced by a PortfolioSizer
 *
 * Volatility values are annualized decimals: 0.20 means 20% annualized volatility.
 * Weight values are fractions of strategy capital: 0.20 means 20% exposure.
 **************************************************************************************/
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
