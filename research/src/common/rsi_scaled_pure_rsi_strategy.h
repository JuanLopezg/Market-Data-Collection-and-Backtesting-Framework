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


// Research-only PureRSI variant whose signal strength follows the current RSI
// (user study, 2026-10-21). Not part of the library's validated strategy catalog.
//
// Entry, exit, universe, ranking and maxActiveSignals are exactly PureRSI's:
// enter when RSI > rsiEntry, exit when RSI < rsiExit. The difference is the size:
// every bar, each active signal is set to a strength from four equal RSI tiers
// between sizeFrom and sizeTo. With sizeFrom 65 and sizeTo 80:
//   RSI >= 80      -> 1.00 (the full equal weight)
//   75 <= RSI < 80 -> 0.75
//   70 <= RSI < 75 -> 0.50
//   RSI < 70       -> 0.25 (also below sizeFrom while the signal is still held)
// Unavailable RSI keeps the existing strength. Whether a changed strength actually
// resizes a held position is decided by the strategy's RebalancePolicy.
class StrategyRsiScaledPureRSI final : public Strategy {
public:
    StrategyRsiScaledPureRSI(
        unsigned int maxActiveSignals,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        unsigned int maxRankingPosition,
        unsigned int rsiLength,
        double rsiEntry,
        double rsiExit,
        double sizeFrom,
        double sizeTo,
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
          sizeFrom_(sizeFrom),
          sizeTo_(sizeTo),
          rsiSpec_{IndicatorKind::RSI, PriceField::Close, rsiLength}
    {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();
        specs.push_back(rsiSpec_);
        return specs;
    }

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

        // Existing signals: exit below rsiExit, otherwise follow the RSI tier.
        std::vector<std::pair<Coin, double>> updates;
        updates.reserve(signalState.activeCount());
        for (const auto& [coin, signal] : signalState.values()) {
            if (signal <= 0.0)
                continue;
            const double rsi = indicators.value(coin, ts, rsiSpec_);
            if (!std::isfinite(rsi))
                continue;
            updates.emplace_back(coin, rsi < rsiExit_ ? 0.0 : strengthFor(rsi));
        }
        for (const auto& [coin, strength] : updates)
            signalState.set(coin, strength);

        if (signalState.activeCount() >= maxActiveSignals_)
            return;
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
                signalState.set(coin, strengthFor(rsi));
        }
    }

private:
    double strengthFor(double rsi) const
    {
        if (rsi >= sizeTo_)
            return 1.0;
        const double tierWidth = (sizeTo_ - sizeFrom_) / 3.0;
        if (rsi >= sizeFrom_ + 2.0 * tierWidth)
            return 0.75;
        if (rsi >= sizeFrom_ + tierWidth)
            return 0.50;
        return 0.25;
    }

    double rsiEntry_ = 80.0;
    double rsiExit_ = 65.0;
    double sizeFrom_ = 65.0;
    double sizeTo_ = 80.0;

    IndicatorSpec rsiSpec_;
};
