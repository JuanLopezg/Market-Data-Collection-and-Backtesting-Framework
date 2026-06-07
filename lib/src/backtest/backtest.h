#pragma once

#include <filesystem>
#include <vector>

#include <sqlite3.h>

#include "data_types.h"
#include "strategy.h"
#include "backtest_context.h"


/**************************************************************************************
 * Type    : Backtester
 * Purpose : Drives the execution of a backtest over historical market data
 **************************************************************************************/
class Backtester {
public:
    explicit Backtester(
        BacktestContext& backtest_context
    );

    /**************************************************************************************
     * Purpose : Execute the main backtest loop
     **************************************************************************************/
    void loop();

    /**************************************************************************************
     * Purpose : Persist all backtest results to disk
     **************************************************************************************/
    void storeResults(std::filesystem::path& backtest_store_path);

    void storeTradesCSV(const std::filesystem::path& csv_store_path);

    /**************************************************************************************
     * Purpose : Copy all still-open trades into trade history
     **************************************************************************************/
    void closeTrades();

private:
    BacktestContext& backtest_context_;

    Timestamp starting_date_ = 0;

    /**************************************************************************************
     * Purpose : Trigger signal calculation for all strategy instances
     **************************************************************************************/
    void calculateSignals(
        const MarketData& marketData,
        Timestamp ts,
        bool live_trading
    );

    /**************************************************************************************
     * Purpose : Update the global backtest context
     **************************************************************************************/
    void updateBacktestContext() {
        backtest_context_.updateConext();
    }

    /**************************************************************************************
     * Purpose : Persist all trades to SQLite
     **************************************************************************************/
    void storeTrades(sqlite3* db);

    /**************************************************************************************
     * Purpose : Persist balance/equity history to SQLite
     **************************************************************************************/
    void storeBalanceEquity(sqlite3* db);

};