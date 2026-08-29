#pragma once

#include <string>

#include "data_types.h"
#include "portfolio_sizing_diagnostics.h"


/**************************************************************************************
 * Type    : VolatilityDiagnosticSnapshot
 * Purpose : Timestamped backtest record of one strategy's volatility-target calculation
 *
 * This is analytics only. It does not participate in sizing, risk or execution.
 **************************************************************************************/
struct VolatilityDiagnosticSnapshot {
    Timestamp timestamp = 0;
    StrategyID strategy_id = 0;
    std::string strategy_name;
    PortfolioSizingDiagnostics sizing;
};
