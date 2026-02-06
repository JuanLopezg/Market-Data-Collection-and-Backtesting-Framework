#include "backtest.h"
#include "logger.h"
#include "time_utils.h"
#include <string>
#include <vector>
#include <utility>
#include <stdexcept>

Backtester::Backtester(const EnrichedData& marketData, Timestamp start, Timestamp end, Strategy& strategy, std::filesystem::path database_path)
    : marketData_(marketData),
      portfolio_(start),
      current_trades_(),
      strategy_(strategy),
      start_(start),
      end_(end),
      database_path_(database_path)
{}

void Backtester::run(){
    LG_INFO("Starting backtest");

    auto beginIt = marketData_.lower_bound(start_);
    auto endIt   = marketData_.upper_bound(end_);
    this->starting_date_ = beginIt->first;

    const size_t totalSteps = std::distance(beginIt, endIt);
    size_t currentStep = 0;

    int lastLoggedPercent = 0;
    constexpr int LOG_STEP = 5;

    for (auto it = marketData_.lower_bound(start_); it != marketData_.end() && it->first <= end_; ++it){

        // Log every 5%
        ++currentStep;
        int percent = static_cast<int>((static_cast<double>(currentStep) / totalSteps) * 100.0);
        if (percent >= lastLoggedPercent + LOG_STEP) {
            LG_INFO("Backtest progress: {}%", percent);
            lastLoggedPercent = percent;
        }

        // Process backtesting data
        Timestamp ts = it->first;
        const CoinBarMap& bars = it->second;

        calculateSignals(bars, ts);
        updatePortfolio();
    }

    if(currentStep > 0){
        LG_INFO("Storing Results:");
        storeResults();
    }
    else{
        LG_INFO("No results to store");
    }

    LG_INFO("Backtest finished");

}
