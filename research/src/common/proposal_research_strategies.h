#pragma once

#include "catalog_research_strategies.h"

#include <array>
#include <chrono>
#include <set>

// Research strategies proposed to the user on 2026-10-22 from the crypto literature
// (time-series trend works, cross-sectional momentum only on the long side). Signals at
// close(T), fills at the next open. Not validated for live trading.

namespace proposal_detail {
inline int dayNumber(Timestamp yyyymmdd)
{
    const std::chrono::year_month_day day{std::chrono::year(static_cast<int>(yyyymmdd / 10000)),
                                          std::chrono::month(static_cast<unsigned>((yyyymmdd / 100) % 100)),
                                          std::chrono::day(static_cast<unsigned>(yyyymmdd % 100))};
    return std::chrono::sys_days{day}.time_since_epoch().count();
}
} // namespace proposal_detail

// A fixed list of symbols (BTCUSDT, ETHUSDT), whatever their liquidity rank.
class FixedUniverse final : public UniverseSelector {
public:
    explicit FixedUniverse(std::set<Coin> symbols) : symbols_(std::move(symbols)) {}
    CoinBarMap select(const CoinBarMap& bars, Timestamp, const IndicatorEngine&) const override
    {
        CoinBarMap selected;
        for (const auto& [coin, bar] : bars)
            if (symbols_.contains(coin))
                selected.emplace(coin, bar);
        return selected;
    }

private:
    std::set<Coin> symbols_;
};

// Donchian trend ensemble: three breakout systems (N = 20, 50, 100). System N turns on when
// the close breaks above the highest high of the previous N bars and off when it closes below
// the lowest low of the previous N/2 bars. Signal = systems on / 3 (0, 1/3, 2/3, 1). States
// are tracked for every asset every day; new positions only from the liquidity universe,
// held ones follow their state until it reaches 0.
class DonchianEnsembleStrategy final : public Strategy {
public:
    DonchianEnsembleStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
                             std::unique_ptr<Ranker> ranker, unsigned int maxRank)
        : Strategy("Donchian_Ensemble", maxSignals, std::move(universe), std::move(ranker), maxRank)
    {
        for (std::size_t i = 0; i < lengths_.size(); ++i) {
            highs_[i] = IndicatorSpec{IndicatorKind::DonchianHigh, PriceField::High, lengths_[i], 1};
            lows_[i] = IndicatorSpec{IndicatorKind::DonchianLow, PriceField::Low, lengths_[i] / 2, 1};
        }
    }

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), highs_.begin(), highs_.end());
        specs.insert(specs.end(), lows_.begin(), lows_.end());
        return specs;
    }

    void updateSignals(const MarketData& market, Timestamp ts, SignalState& signals,
                       const IndicatorEngine& indicators) const override
    {
        const auto slice = market.find(ts);
        if (slice == market.end())
            return;
        for (const auto& [coin, bar] : slice->second) {
            auto& on = states_[coin];
            for (std::size_t i = 0; i < lengths_.size(); ++i) {
                const double high = indicators.value(coin, ts, highs_[i]);
                const double low = indicators.value(coin, ts, lows_[i]);
                if (std::isfinite(high) && bar.close > high)
                    on[i] = true;
                else if (std::isfinite(low) && bar.close < low)
                    on[i] = false;
            }
        }
        // Held signals follow their state (an asset without a bar today keeps its signal).
        std::vector<std::pair<Coin, double>> updates;
        for (const auto& [coin, value] : signals.values())
            if (value > 0.0 && slice->second.contains(coin))
                updates.emplace_back(coin, strength(coin));
        for (const auto& [coin, value] : updates)
            signals.set(coin, value);
        if (signals.activeCount() >= maxActiveSignals_)
            return;
        const auto tradable = universeSelector_->select(slice->second, ts, indicators);
        for (const auto& candidate : ranker_->rank(tradable, ts, indicators)) {
            if (signals.activeCount() >= maxActiveSignals_)
                break;
            if (!signals.isActive(candidate.coin) && strength(candidate.coin) > 0.0)
                signals.set(candidate.coin, strength(candidate.coin));
        }
    }

private:
    double strength(const Coin& coin) const
    {
        const auto it = states_.find(coin);
        if (it == states_.end())
            return 0.0;
        return static_cast<double>(std::count(it->second.begin(), it->second.end(), true)) / 3.0;
    }

    std::array<unsigned int, 3> lengths_{20, 50, 100};
    std::array<IndicatorSpec, 3> highs_, lows_;
    mutable std::unordered_map<Coin, std::array<bool, 3>> states_;
};

// Moving-average trend on a fixed list (BTC and ETH): long while SMA(fast) > SMA(slow).
class FixedListSmaTrendStrategy final : public ConditionResearchStrategy {
public:
    FixedListSmaTrendStrategy(std::set<Coin> symbols, unsigned int fastLength, unsigned int slowLength)
        : ConditionResearchStrategy("SMA_Trend_BTC_ETH", static_cast<unsigned int>(symbols.size()),
                                    std::make_unique<FixedUniverse>(symbols),
                                    std::make_unique<IndicatorRanker>(IndicatorSpec{IndicatorKind::SMA, PriceField::Volume, 25}, true),
                                    1000000),
          fast_{IndicatorKind::SMA, PriceField::Close, fastLength}, slow_{IndicatorKind::SMA, PriceField::Close, slowLength} {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), {fast_, slow_});
        return specs;
    }

private:
    bool entersNow(const Coin& coin, const BarData&, const MarketData&, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const double fast = indicators.value(coin, ts, fast_), slow = indicators.value(coin, ts, slow_);
        return std::isfinite(fast) && std::isfinite(slow) && fast > slow;
    }
    bool exitsNow(const Coin& coin, const BarData&, const MarketData&, Timestamp ts,
                  const IndicatorEngine& indicators) const override
    {
        const double fast = indicators.value(coin, ts, fast_), slow = indicators.value(coin, ts, slow_);
        return std::isfinite(fast) && std::isfinite(slow) && fast <= slow;
    }
    IndicatorSpec fast_, slow_;
};

// Long-only weekly momentum: every `rebalanceDays` the holdings become the top `count` of the
// liquidity universe by ROC(rocLength), only while the benchmark closes above SMA(btcLength)
// (otherwise cash). Between rebalances a holding exits when its close falls `stopFraction`
// below its signal-day close (close-based stop, sold at the next open).
class WeeklyMomentumLongStrategy final : public Strategy {
public:
    WeeklyMomentumLongStrategy(unsigned int count, std::unique_ptr<UniverseSelector> universe, unsigned int rocLength,
                               unsigned int rebalanceDays, double stopFraction, unsigned int btcLength, std::string benchmark)
        : Strategy("Momentum_Long_Weekly", count, std::move(universe),
                   std::make_unique<IndicatorRanker>(IndicatorSpec{IndicatorKind::ROC, PriceField::Close, rocLength}, true),
                   1000000),
          rebalanceDays_(rebalanceDays), stopFraction_(stopFraction),
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, btcLength}, benchmark_(std::move(benchmark)) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(benchmarkAverage_);
        return specs;
    }

    void updateSignals(const MarketData& market, Timestamp ts, SignalState& signals,
                       const IndicatorEngine& indicators) const override
    {
        const auto slice = market.find(ts);
        if (slice == market.end())
            return;
        const int today = proposal_detail::dayNumber(ts);
        if (lastRebalance_ == 0 || today - lastRebalance_ >= static_cast<int>(rebalanceDays_)) {
            lastRebalance_ = today;
            std::vector<Coin> previous;
            for (const auto& [coin, value] : signals.values())
                if (value != 0.0)
                    previous.push_back(coin);
            for (const auto& coin : previous)
                signals.set(coin, 0.0);
            entries_.clear();
            const auto benchmarkBar = slice->second.find(benchmark_);
            const double average = indicators.value(benchmark_, ts, benchmarkAverage_);
            if (benchmarkBar == slice->second.end() || !std::isfinite(average) || benchmarkBar->second.close <= average)
                return;
            const auto tradable = universeSelector_->select(slice->second, ts, indicators);
            for (const auto& candidate : ranker_->rank(tradable, ts, indicators)) {
                if (signals.activeCount() >= maxActiveSignals_)
                    break;
                signals.set(candidate.coin, 1.0);
                entries_[candidate.coin] = slice->second.at(candidate.coin).close;
            }
            return;
        }
        std::vector<Coin> stopped;
        for (const auto& [coin, value] : signals.values()) {
            const auto bar = slice->second.find(coin);
            const auto entry = entries_.find(coin);
            if (value != 0.0 && bar != slice->second.end() && entry != entries_.end() &&
                bar->second.close < entry->second * (1.0 - stopFraction_))
                stopped.push_back(coin);
        }
        for (const auto& coin : stopped)
            signals.set(coin, 0.0);
    }

private:
    unsigned int rebalanceDays_;
    double stopFraction_;
    IndicatorSpec benchmarkAverage_;
    std::string benchmark_;
    mutable int lastRebalance_ = 0;
    mutable std::unordered_map<Coin, double> entries_;
};
