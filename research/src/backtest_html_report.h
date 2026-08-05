#pragma once

#include "backtest_metrics.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// One aggregated row for one value of a parameter in a full-grid sensitivity
// study. Each metric is the median across all valid combinations that share
// parameterValue.
struct ParameterSensitivityPoint {
    double parameterValue = 0.0;
    double medianFinalReturnPercent = 0.0;
    double medianMaxDrawdownPercent = 0.0;
    double medianTradeCount = 0.0;
    std::size_t positiveCombinationCount = 0U;
    std::size_t totalCombinationCount = 0U;
    std::size_t zeroTradeCombinationCount = 0U;
};

struct ParameterSensitivityReport {
    std::string parameterName;
    std::string displayName;
    std::vector<ParameterSensitivityPoint> points;
};

// Writes one self-contained, non-interactive HTML report. The equity curve and
// sensitivity charts are inline SVG, so no JavaScript, chart library, CSV, or
// external image is required.
bool writeBacktestHtmlReport(
    const std::filesystem::path& outputPath,
    const BacktestMetrics& metrics,
    const std::vector<std::pair<Balance, Equity>>& balanceEquityHistoric,
    const MarketData& marketData,
    const std::vector<ParameterSensitivityReport>& parameterSensitivity = {}
);
