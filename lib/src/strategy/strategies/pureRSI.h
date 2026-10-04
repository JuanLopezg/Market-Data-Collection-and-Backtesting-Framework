#pragma once

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "data_types.h"
#include "indicator_engine.h"
#include "indicator_spec.h"
#include "market_filter.h"
#include "ranker.h"
#include "strategy.h"
#include "universe_selector.h"


/**************************************************************************************
 * Type    : StrategyPureRSI
 * Purpose : Long high-RSI strength continuation strategy
 *
 * Signal meaning:
 *   +1.0 = strategy wants full long exposure
 *    0.0 = strategy wants no exposure
 *
 * Entry:
 *   RSI(RSILen) > RSIEntry
 *
 * Exit:
 *   RSI(RSILen) < RSIExit
 *
 * Notes:
 *   - Signals are persistent; they remain +1 until the exit condition is met.
 *   - Existing signals are checked for exit before filters/universe/ranking are applied.
 *   - Market filters, universe selection and ranking only control NEW signals.
 *   - Position sizing, money, orders, commissions and PnL do not belong here.
 **************************************************************************************/
class StrategyPureRSI final : public Strategy {
public:
    StrategyPureRSI(
        unsigned int maxActiveSignals,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        unsigned int maxRankingPosition,
        unsigned int rsiLength,
        double rsiEntry,
        double rsiExit,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "Pure_RSI",
              maxActiveSignals,
              std::move(universeSelector),
              std::move(ranker),
              maxRankingPosition,
              std::move(marketFilters)
          ),
          rsiEntry_(rsiEntry),
          rsiExit_(rsiExit),
          rsiSpec_{IndicatorKind::RSI, PriceField::Close, rsiLength}
    {}

    /**************************************************************************************
     * Purpose : Return all indicators required by PureRSI and its shared components
     **************************************************************************************/
    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();
        specs.push_back(rsiSpec_);
        return specs;
    }

    /**************************************************************************************
     * Purpose : Update PureRSI signals for one timestamp
     *
     * Order of operations intentionally mirrors the old strategy behavior:
     *   1. Existing active signals may exit regardless of entry filters.
     *   2. Market filters are checked only before looking for new signals.
     *   3. The current universe is selected and ranked.
     *   4. New +1 signals are added until maxActiveSignals_ is reached.
     **************************************************************************************/
    void updateSignals(
        const MarketData& marketData,
        Timestamp ts,
        SignalState& signalState,
        const IndicatorEngine& indicators
    ) const override
    {
        const auto tsIt = marketData.find(ts);
        if (tsIt == marketData.end())
            return;

        const CoinBarMap& bars = tsIt->second;

        // Exit logic is evaluated for every active signal before entry filters/ranking.
        std::vector<Coin> signalsToExit;
        signalsToExit.reserve(signalState.activeCount());

        for (const auto& [coin, signal] : signalState.values()) {
            if (signal <= 0.0)
                continue;

            const double rsi = indicators.value(coin, ts, rsiSpec_);
            if (std::isfinite(rsi) && rsi < rsiExit_)
                signalsToExit.push_back(coin);
        }

        // SignalState cannot be modified while iterating over its internal map.
        for (const Coin& coin : signalsToExit)
            signalState.set(coin, 0.0);

        if (signalState.activeCount() >= maxActiveSignals_)
            return;

        // Filters only prevent new signals; they never force an existing signal to exit.
        if (!marketFiltersPass(marketData, ts, indicators))
            return;

        CoinBarMap tradableBars = universeSelector_->select(bars, ts, indicators);
        if (tradableBars.empty())
            return;

        const RankedUniverse ranked = ranker_->rank(tradableBars, ts, indicators);
        unsigned int rankingPosition = 0;

        for (const auto& rankedCoin : ranked) {
            ++rankingPosition;

            if (signalState.activeCount() >= maxActiveSignals_ ||
                rankingPosition > maxRankingPosition_)
                break;

            const Coin& coin = rankedCoin.coin;
            if (signalState.isActive(coin))
                continue;

            const double rsi = indicators.value(coin, ts, rsiSpec_);
            if (std::isfinite(rsi) && rsi > rsiEntry_)
                signalState.set(coin, 1.0);
        }
    }

private:
    double rsiEntry_ = 80.0;
    double rsiExit_ = 70.0;

    IndicatorSpec rsiSpec_;
};
