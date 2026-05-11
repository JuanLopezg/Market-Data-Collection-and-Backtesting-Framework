#pragma once

#include <filesystem>
#include <map>

#include "data_types.h"


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