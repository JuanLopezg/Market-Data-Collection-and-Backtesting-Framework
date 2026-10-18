#pragma once

#include "backtester.h"
#include "data_types.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

// Writes an interactive Plotly chart comparing account balance and equity.
void printBalanceEquityChart(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    const MarketData& marketData,
    const std::vector<VolatilityDiagnosticSnapshot>& volatilityDiagnostics,
    std::size_t rollingVolatilityWindow,
    double periodsPerYear,
    const std::string& outputHtml =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity.html"
);

// Writes date,balance,equity per market bar (the same series as the chart above).
void storeBalanceEquityCSV(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    const MarketData& marketData,
    const std::filesystem::path& outputCsv
);
