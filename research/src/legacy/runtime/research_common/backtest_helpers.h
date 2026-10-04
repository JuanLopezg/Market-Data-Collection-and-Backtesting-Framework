#pragma once

#include "backtest.h"
#include "data_types.h"

#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

// Writes an interactive Plotly candlestick chart with entry and exit markers.
void printCoinTradesPlotlyChart(
    const MarketData& marketData,
    const std::map<TradeID, Trade>& tradesHistory,
    const std::string& coin = "AAVE",
    const std::string& outputHtml =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/aave_trades.html"
);

// Writes an interactive Plotly chart comparing account balance and equity.
void printBalanceEquityChart(
    const std::vector<std::pair<Balance, Equity>>& eqbal,
    const MarketData& marketData,
    const std::string& outputHtml =
        "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity.html"
);

// Logs all trades in a backtest trade-history map.
void printTradesHistory(const std::map<TradeID, Trade>& tradesHistory);

// Copies a CSV while scaling LUNC OHLC values by 1,000.
void scaleLuncOHLCBy1000(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputPath
);

// Copies CSV rows whose first-column date is in [startDate, endDateExclusive).
void filterCSVByDateRange(
    const std::filesystem::path& inputPath,
    const std::filesystem::path& outputPath,
    const std::string& startDate,
    const std::string& endDateExclusive
);
