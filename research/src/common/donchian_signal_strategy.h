#pragma once

#include "indicator_engine.h"
#include "strategy.h"

#include <cmath>
#include <utility>

// Research-only Donchian experiment using CURRENT close-signal/next-open economics.
// This is not an addition to the library's validated strategy catalog.
class DonchianSignalStrategy final : public Strategy {
public:
    DonchianSignalStrategy(
        unsigned int maxSignals,
        std::unique_ptr<UniverseSelector> universe,
        std::unique_ptr<Ranker> ranker,
        unsigned int maxRankingPosition,
        unsigned int lookback,
        bool useBenchmarkFilter,
        unsigned int benchmarkLength,
        std::string benchmark
    )
        : Strategy("DonchianBreakout", maxSignals, std::move(universe),
                   std::move(ranker), maxRankingPosition),
          high_{IndicatorKind::DonchianHigh, PriceField::High, lookback, 1},
          midpoint_{IndicatorKind::DonchianMid, PriceField::Close, lookback},
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, benchmarkLength},
          useBenchmarkFilter_(useBenchmarkFilter), benchmark_(std::move(benchmark))
    {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(high_);
        specs.push_back(midpoint_);
        if (useBenchmarkFilter_)
            specs.push_back(benchmarkAverage_);
        return specs;
    }

    void updateSignals(const MarketData& market, Timestamp ts, SignalState& signals,
                       const IndicatorEngine& indicators) const override
    {
        const auto slice = market.find(ts);
        if (slice == market.end())
            return;
        const auto& bars = slice->second;
        std::vector<Coin> exits;
        for (const auto& [coin, signal] : signals.values()) {
            (void)signal;
            const auto bar = bars.find(coin);
            const double midpoint = indicators.value(coin, ts, midpoint_);
            if (bar != bars.end() && std::isfinite(midpoint) && bar->second.close < midpoint)
                exits.push_back(coin);
        }
        // Filters gate entries only. Exits are intents executed by CURRENT Backtester
        // at the next available open, unlike the frozen experiment's same-close fills.
        for (const auto& coin : exits)
            signals.set(coin, 0);
        if (signals.activeCount() >= maxActiveSignals_ || !marketFiltersPass(market, ts, indicators))
            return;
        if (useBenchmarkFilter_) {
            const auto benchmark = bars.find(benchmark_);
            const double average = indicators.value(benchmark_, ts, benchmarkAverage_);
            if (benchmark == bars.end() || !std::isfinite(average) || average <= 0
                || benchmark->second.close <= average)
                return;
        }
        const auto tradable = universeSelector_->select(bars, ts, indicators);
        unsigned int rank = 0;
        for (const auto& candidate : ranker_->rank(tradable, ts, indicators)) {
            if (++rank > maxRankingPosition_ || signals.activeCount() >= maxActiveSignals_)
                break;
            if (signals.isActive(candidate.coin))
                continue;
            const double high = indicators.value(candidate.coin, ts, high_);
            if (std::isfinite(high) && high > 0 && tradable.at(candidate.coin).close > high)
                signals.set(candidate.coin, 1);
        }
    }

private:
    IndicatorSpec high_;
    IndicatorSpec midpoint_;
    IndicatorSpec benchmarkAverage_;
    bool useBenchmarkFilter_;
    std::string benchmark_;
};
