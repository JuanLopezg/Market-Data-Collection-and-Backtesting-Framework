#include "backtest.h"
#include "logger.h"
#include "time_utils.h"


Backtester::Backtester(const EnrichedData& marketData,Timestamp start,Timestamp end, Strategy strategy)
    : marketData_(marketData),
      start_(start),
      end_(end),
      portfolio_(start),
      strategy_(strategy)
{};


void Backtester::run(){
    LG_INFO("Starting backtest");

    for (auto it = marketData_.lower_bound(start_); it != marketData_.end() && it->first <= end_; ++it){
        Timestamp ts = it->first;
        const CoinBarMap& bars = it->second;

        calculateSignals(bars, ts);
        updatePortfolio();
    }

    LG_INFO("Storing Results:");
    storeResults();

    LG_INFO("Backtest finished");

}