#include "backtest.h"
#include "logger.h"
#include "time_utils.h"
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>


/**************************************************************************************
 * Type    : Backtester
 * Purpose : Orchestrates the execution of a backtest over historical market data
 *
 * This class drives the backtest loop. It iterates through historical timestamps,
 * invokes signal generation on all strategy instances, updates the backtest context,
 * and tracks execution progress.
 **************************************************************************************/
Backtester::Backtester(BacktestContext& backtest_context)
    : backtest_context_(backtest_context)
{}


/**************************************************************************************
 * Purpose : Trigger signal calculation for all strategy instances at a given timestamp
 *
 * This method forwards the current market snapshot to each strategy instance,
 * allowing strategies to generate new trades or update existing ones.
 *
 * Args    : bars - market data for all coins at the current timestamp
 *           ts   - current timestamp
 * Return  : None
 **************************************************************************************/
void Backtester::calculateSignals(const EnrichedData& marketData, Timestamp ts, bool live_trading){
    for (auto& strategy_instance : backtest_context_.GetStrategyPortfolio()) {
        strategy_instance.calculateSignals(
            marketData,
            ts,
            this->backtest_context_.GetLastTradeId(),
            this->backtest_context_.IsLiveTrading()
        );
    }
}


/**************************************************************************************
 * Purpose : Execute the main backtest loop
 *
 * Iterates sequentially over the enriched historical market data, processes strategy
 * signals for each timestamp, updates the global backtest context, and logs progress.
 *
 * The loop assumes time-ordered market data and performs a full portfolio update
 * at each step.
 *
 * Args    : None
 * Return  : None
 **************************************************************************************/
void Backtester::loop(){
    LG_INFO("Starting backtest");

    const EnrichedData& marketData = this->backtest_context_.GetMarketData();
    assert(!marketData.empty());

    StrategyPortfolio& strategy_portfolio =
        this->backtest_context_.GetStrategyPortfolio();

    // Record the first timestamp of the backtest
    this->starting_date_ = marketData.begin()->first;

    const size_t totalSteps = marketData.size();
    size_t currentStep = 0;
    int lastLoggedPercent = 0;
    constexpr int LOG_STEP = 5;

    for (auto it = marketData.begin(); it != marketData.end(); ++it){

        // Log progress at fixed percentage intervals
        ++currentStep;
        int percent = static_cast<int>(
            (static_cast<double>(currentStep) / totalSteps) * 100.0
        );

        if (percent >= lastLoggedPercent + LOG_STEP) {
            LG_INFO("Backtest progress: {}%", percent);
            lastLoggedPercent = percent;
        }

        // Process backtesting data for the current timestamp
        Timestamp ts = it->first;
        calculateSignals(marketData, ts, this->backtest_context_.IsLiveTrading());
        updateBacktestContext();

    }

    LG_INFO("Backtest finished");
}
