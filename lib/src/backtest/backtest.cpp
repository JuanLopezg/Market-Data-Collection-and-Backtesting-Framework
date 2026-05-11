#include "backtest.h"

#include <cassert>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "logger.h"
#include "time_utils.h"


/**************************************************************************************
 * Purpose : Construct Backtester
 **************************************************************************************/
Backtester::Backtester(BacktestContext& backtest_context)
    : backtest_context_(backtest_context)
{}


/**************************************************************************************
 * Purpose : Trigger signal calculation for all strategy instances at a timestamp
 **************************************************************************************/
void Backtester::calculateSignals(
    const MarketData& marketData,
    Timestamp ts,
    bool live_trading
)
{
    const IndicatorEngine& indicators =
        backtest_context_.GetIndicatorEngine();

    for (auto& strategyInstance : backtest_context_.GetStrategyPortfolio()) {
        strategyInstance.calculateSignals(
            marketData,
            ts,
            backtest_context_.GetLastTradeId(),
            live_trading,
            indicators
        );
    }
}


/**************************************************************************************
 * Purpose : Execute the main backtest loop
 **************************************************************************************/
void Backtester::loop()
{
    LG_INFO("Starting backtest");

    const MarketData& marketData = backtest_context_.GetMarketData();
    assert(!marketData.empty());

    starting_date_ = marketData.begin()->first;

    const std::size_t totalSteps = marketData.size();
    std::size_t currentStep = 0;

    int lastLoggedPercent = 0;
    constexpr int LOG_STEP = 5;

    for (auto it = marketData.begin(); it != marketData.end(); ++it) {
        ++currentStep;

        const int percent = static_cast<int>(
            (static_cast<double>(currentStep) /
             static_cast<double>(totalSteps)) * 100.0
        );

        if (percent >= lastLoggedPercent + LOG_STEP) {
            LG_INFO("Backtest progress: {}%", percent);
            lastLoggedPercent = percent;
        }

        const Timestamp ts = it->first;

        calculateSignals(
            marketData,
            ts,
            backtest_context_.IsLiveTrading()
        );

        updateBacktestContext();
    }

    LG_INFO("Backtest finished");
}