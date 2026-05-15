#pragma once

#include <string>
#include <vector>

#include "market_filter.h"
#include "indicator_spec.h"


/**************************************************************************************
 * Type    : BenchmarkAboveSMAFilter
 * Purpose : Market regime filter based on a benchmark coin being above its SMA
 *
 * Example:
 *   BTC close > SMA(Close, 50)
 *
 * If the benchmark is below the SMA, strategies using this filter will not open
 * new trades.
 **************************************************************************************/
class BenchmarkAboveSMAFilter : public MarketFilter {
private:
    Coin benchmarkCoin_;
    IndicatorSpec smaSpec_;

public:
    /**************************************************************************************
     * Purpose : Construct benchmark-above-SMA filter
     *
     * Args:
     *   benchmarkCoin - benchmark symbol, for example "BTC"
     *   smaLength     - SMA length, for example 50
     *   source        - price field used for the SMA, usually Close
     **************************************************************************************/
    BenchmarkAboveSMAFilter(
        Coin benchmarkCoin,
        unsigned int smaLength,
        PriceField source = PriceField::Close
    );

    bool passes(
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const override;

    std::vector<IndicatorSpec> requiredIndicators() const override;
};