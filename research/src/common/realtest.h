#pragma once

#include <filesystem>
#include <map>

#include "data_types.h"
#include "portfolio_sizer.h"


bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    bool showAllTrades
);

bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    bool showAllTrades,
    const std::filesystem::path& mismatchesCsvPath
);

bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory
);
// Volatility-target validation: compare only the strategic trade campaign boundaries.
// Quantity and PnL magnitude are intentionally ignored because sizing/rebalancing differs.
bool compareBacktestCampaigns(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    const std::filesystem::path& comparisonCsvPath,
    double priceTolerancePercent = 0.01
);

// Choose the RealTest validation automatically from the configured sizing method.
// EqualWeight keeps the exhaustive legacy comparison; VolatilityTarget compares
// strategic campaign boundaries while quantity/PnL magnitude remain intentionally free.
bool compareBacktestBySizing(
    PortfolioSizerKind sizingKind,
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    const std::filesystem::path& comparisonCsvPath
);
