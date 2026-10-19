#pragma once

#include "indicator_engine.h"
#include "strategy_signal_engine.h"
#include "validated/pure_rsi.h"

#include <initializer_list>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace strategy_test {

constexpr Timestamp thirdClose = 20240103;

struct RsiSettings {
    unsigned max_active_signals = 2;
    unsigned max_ranking_position = 100;
    unsigned length = 2;
    double entry = 75;
    double exit = 25;
};

inline std::unique_ptr<StrategyPureRSI> makeRsi(
    RsiSettings settings = {},
    std::unique_ptr<UniverseSelector> universe = {},
    std::unique_ptr<Ranker> ranker = {},
    std::vector<std::unique_ptr<MarketFilter>> filters = {})
{
    return std::make_unique<StrategyPureRSI>(settings.max_active_signals,
        std::move(universe), std::move(ranker), settings.max_ranking_position,
        settings.length, settings.entry, settings.exit, std::move(filters));
}

inline void addCloses(OHLCVData& raw, const Coin& coin, const std::vector<double>& closes,
                      double quoteVolume = std::numeric_limits<double>::quiet_NaN(),
                      double baseVolume = 1)
{
    for (std::size_t i = 0; i < closes.size(); ++i) {
        const double close = closes[i];
        raw.data[coin][20240101u + static_cast<Timestamp>(i)] =
            {close, close * 1.1, close * 0.9, close, baseVolume, quoteVolume};
    }
}

inline void update(const Strategy& strategy, const OHLCVData& raw, const MarketData& market,
                   Timestamp date, SignalState& signals)
{
    IndicatorEngine indicators;
    indicators.precompute(raw, strategy.requiredIndicators());
    strategy.updateSignals(market, date, signals, indicators);
}

inline StrategySignalEngine singleEngine(RsiSettings settings = {})
{
    StrategySignalPortfolio portfolio;
    portfolio.emplace_back(7, makeRsi(settings));
    return StrategySignalEngine(std::move(portfolio));
}

inline StrategySignalEngine twoStrategyEngine()
{
    StrategySignalPortfolio portfolio;
    portfolio.emplace_back(7, makeRsi());
    portfolio.emplace_back(9, makeRsi({.entry = 85}));
    return StrategySignalEngine(std::move(portfolio));
}

inline StrategyIntentBatch checkpoint(Timestamp date,
                                      std::initializer_list<StrategySignalIntent> intents)
{
    StrategyIntentBatch batch;
    batch.timestamp = date;
    batch.strategies = intents;
    return batch;
}

inline const StrategySignalIntent& intent(const StrategyIntentBatch& batch, StrategyID id)
{
    for (const auto& record : batch.strategies)
        if (record.strategy_id == id) return record;
    throw std::out_of_range("Expected strategy intent is missing");
}

} // namespace strategy_test
