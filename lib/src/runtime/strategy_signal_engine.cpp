#include "strategy_signal_engine.h"

#include <stdexcept>
#include <unordered_set>
#include <utility>


StrategySignalEngine::StrategySignalEngine(StrategySignalPortfolio strategies)
    : strategies_(std::move(strategies))
{
    if (strategies_.empty())
        throw std::invalid_argument("StrategySignalEngine requires at least one strategy");

    std::unordered_set<StrategyID> ids;
    for (const StrategySignalInstance& strategy : strategies_) {
        if (!ids.insert(strategy.id()).second)
            throw std::invalid_argument("StrategySignalEngine strategy ids must be unique");

        const auto specs = strategy.requiredIndicators();
        required_indicators_.insert(
            required_indicators_.end(),
            specs.begin(),
            specs.end()
        );
    }
}


StrategyIntentBatch StrategySignalEngine::onBarClose(
    const OHLCVData& rawData,
    const MarketData& marketData,
    Timestamp ts
)
{
    if (ts == 0)
        throw std::invalid_argument("Strategy signal timestamp must be non-zero");
    if (last_timestamp_ != 0 && ts <= last_timestamp_)
        throw std::logic_error("Strategy signal timestamps must be strictly increasing");
    if (marketData.find(ts) == marketData.end())
        throw std::out_of_range("Strategy signal timestamp is missing from market data");

    // The service receives only already-closed slices, so recomputing over the locally
    // accumulated raw history cannot introduce lookahead. This is intentionally simple
    // for the first service boundary; incremental indicator updates are a later optimization.
    indicators_.precompute(rawData, required_indicators_);

    StrategyIntentBatch batch;
    batch.timestamp = ts;
    batch.strategies.reserve(strategies_.size());

    for (StrategySignalInstance& strategy : strategies_) {
        strategy.updateSignals(marketData, ts, indicators_);

        StrategySignalIntent intent;
        intent.strategy_id = strategy.id();
        intent.strategy_name = strategy.name();
        intent.signals = strategy.signalState().values();
        batch.strategies.push_back(std::move(intent));
    }

    last_timestamp_ = ts;
    return batch;
}
