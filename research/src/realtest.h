#include <filesystem>
#include "backtest.h"
#include "ranker.h"
#include "database_utils.h"
#include "data_types.h"
#include "csv_utils.h"
#include "logger.h"

#include <sqlite3.h>
#include <iostream>
#include <map>
#include <string>
#include <fstream>
#include <vector>
#include <utility>

#include <cstdio>
#include <algorithm>
#include <cstdlib>
#include <filesystem>



bool compareBacktests(
    std::filesystem::path& realtest_trades,
    std::map<TradeID, Trade>& backtesterTradesHistory,
    bool showAllTrades
);