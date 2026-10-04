#include "benchmark_above_sma_filter.h"

#include <cmath>
#include <utility>

#include "indicator_engine.h"


BenchmarkAboveSMAFilter::BenchmarkAboveSMAFilter(
    Coin benchmarkCoin,
    unsigned int smaLength,
    PriceField source
)
    : benchmarkCoin_(std::move(benchmarkCoin)),
      smaSpec_{IndicatorKind::SMA, source, smaLength}
{}


bool BenchmarkAboveSMAFilter::passes(
    const MarketData& marketData,
    Timestamp ts,
    const IndicatorEngine& indicators
) const
{
    const auto tsIt = marketData.find(ts);
    if (tsIt == marketData.end()) return false;

    const auto coinIt = tsIt->second.find(benchmarkCoin_);
    if (coinIt == tsIt->second.end()) return false;

    const BarData& benchmarkBar = coinIt->second;

    const double sma = indicators.value(
        benchmarkCoin_,
        ts,
        smaSpec_
    );

    return
        std::isfinite(sma) &&
        sma > 0.0 &&
        benchmarkBar.close > sma;
}


std::vector<IndicatorSpec> BenchmarkAboveSMAFilter::requiredIndicators() const
{
    return {smaSpec_};
}