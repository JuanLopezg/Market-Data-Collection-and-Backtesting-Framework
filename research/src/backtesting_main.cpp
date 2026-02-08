#include "backtest.h"
#include "strategy_high_breakout.h"
#include "ranker.h"
#include "database.h"

int main(int argc, char** argv) {

    EnrichedData market_data;

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
    double riskPerTrade = 0.1;
    std::unique_ptr<Ranker> ranker = std::make_unique<VolumeRanker>();
    double commissionEntryPctg = feeMaker;
    double commissionExitPctg = feeTaker;

    strategies.push_back(
        std::make_unique<StrategyHighBreakout>(
            maxPosOpen,
            riskPerTrade,
            std::move(ranker),
            commissionEntryPctg,
            commissionExitPctg
        )
    );

    //-----------------------------------------------------
    // Another strategy

    /***********************************************************************************************************************/


    BacktestContext context = BacktestContext(market_data, strategies, balance, equity, last_trade_id, feeMaker, feeTaker);
    Backtester tester = Backtester(context);
    
    tester.loop();

    std::filesystem::path backtest_store_dir = "/c/Users/Juan/Documents/Python/algoTrading/research/backtests/";
    std::filesystem::path backtest_store_path = generateBacktestDbPath(backtest_store_dir);

    tester.storeResults(backtest_store_path);

    return 0;
}
