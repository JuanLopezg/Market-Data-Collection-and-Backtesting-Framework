#include "backtest.h"
#include "strategy_high_breakout.h"
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
#include <string>
#include <utility>
#include <iostream>


int main(int argc, char** argv)
{
    // ----------------------------------------------------
    // Minimal logger setup (console only)
    // ----------------------------------------------------
    Logger::Instance().Setup(
        true,   // debug enabled
        false,  // quiet
        "",     // file appender
        "",     // rolling appender
        true    // include header
    );

    std::string path = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/databases/top_20_database.db";

    LG_INFO("Database loading started");
    OHLCVData ohlcvData = loadDatabase(path, 00000000);
    LG_INFO("Database loaded successful");

    EnrichedData market_data = buildEnriched(ohlcvData);
    LG_INFO("Enriched data generation finished");

    unsigned int last_trade_id = 0;
    double balance = 100000.0;
    double equity = balance;
    double feeTaker = 0.045 / 100.0; // HL fees
    double feeMaker = 0.015 / 100.0; // HL fees

    /***********************************************************************************************************************/
    std::vector<std::unique_ptr<Strategy>> strategies;

    //-----------------------------------------------------
    // High Breakout
    unsigned int maxPosOpen = 10;
    double riskPerTrade = 0.005;
    std::unique_ptr<Ranker> ranker = std::make_unique<VolumeRanker>();
    double commissionEntryFactor = feeMaker;
    double commissionExitFactor = feeTaker;
    unsigned int maxRankingPosition = 20;

    strategies.push_back(
        std::make_unique<StrategyHighBreakout>(
            maxPosOpen,
            riskPerTrade,
            std::move(ranker),
            commissionEntryFactor,
            commissionExitFactor,
            maxRankingPosition
        )
    );

    //-----------------------------------------------------
    // Another strategy

    /***********************************************************************************************************************/


    BacktestContext context = BacktestContext(market_data, strategies, balance, equity, last_trade_id, feeMaker, feeTaker);
    Backtester tester = Backtester(context);
    
    LG_INFO("Starting loop");
    tester.loop();
    LG_INFO("Finished loop");

    std::filesystem::path backtest_store_dir = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/";
    std::filesystem::path backtest_store_path = generateBacktestDbPath(backtest_store_dir);

    tester.storeResults(backtest_store_path);

    const std::string filename = "/mnt/c/Users/Juan/Documents/Python/algoTrading/storage/backtests/balance_equity_curve.csv";
    saveCurveToCSV(context.GetBalanceEquityHistoric(), filename);

    return 0;
}
