#pragma once

#include "indicator_engine.h"
#include "strategy.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

// Shared holding-period lifecycle for research signals. Filled campaigns come
// from CURRENT TradeRecorder; this component never creates trades or money.
class TimedResearchStrategy : public Strategy {
public:
    bool requiresTradeObservations() const override { return true; }

    void observeTrades(const std::vector<TradeRecord>& trades) const override
    {
        held_.clear();
        for (const auto& trade : trades) {
            if (!trade.exited)
                held_.emplace(trade.coin, trade);
        }
    }

    double entryStopPrice(const Coin& coin) const override { return entryStops_.at(coin); }
    double protectiveStopPrice(const Coin& coin) const override
    {
        const auto it = protectiveStops_.find(coin);
        return it == protectiveStops_.end() ? 0.0 : it->second;
    }

    void updateSignals(const MarketData& market, Timestamp ts, SignalState& signals,
                       const IndicatorEngine& indicators) const override
    {
        const auto slice = market.find(ts);
        if (slice == market.end())
            return;
        std::vector<Coin> previous;
        for (const auto& [coin, value] : signals.values()) {
            (void)value;
            previous.push_back(coin);
        }
        for (const auto& coin : previous)
            signals.set(coin, 0.0);
        entryStops_.clear();
        protectiveStops_.clear();
        for (const auto& [coin, trade] : held_) {
            unsigned int observedBars = 0;
            for (auto day = market.lower_bound(trade.start); day != market.end() && day->first <= ts; ++day) {
                if (day->second.contains(coin))
                    ++observedBars;
            }
            // Stop-entry experiments exclude their entry bar from exit decisions,
            // including heldBars=1. Market-entry studies may exit at its close.
            const bool canExit = slice->second.contains(coin) &&
                (!excludeEntryExit_ || ts > trade.start);
            if (!canExit || observedBars < heldBars_)
                signals.set(coin, direction_);
        }
        if (!marketFiltersPass(market, ts, indicators))
            return;
        const auto tradable = universeSelector_->select(slice->second, ts, indicators);
        unsigned int rank = 0;
        std::size_t reserved = held_.size();
        for (const auto& candidate : ranker_->rank(tradable, ts, indicators)) {
            if (++rank > maxRankingPosition_ || reserved >= maxActiveSignals_)
                break;
            if (held_.contains(candidate.coin))
                continue;
            const auto bar = slice->second.find(candidate.coin);
            if (bar == slice->second.end() || !std::isfinite(bar->second.close) || bar->second.close <= 0.0)
                continue;
            if (qualifies(candidate.coin, bar->second, market, ts, indicators)) {
                signals.set(candidate.coin, direction_);
                ++reserved;
            }
        }
    }

protected:
    TimedResearchStrategy(std::string name, unsigned int maxSignals,
        std::unique_ptr<UniverseSelector> universe, std::unique_ptr<Ranker> ranker,
        unsigned int maxRank, unsigned int heldBars, double direction = 1.0,
        bool excludeEntryExit = false)
        : Strategy(std::move(name), maxSignals, std::move(universe), std::move(ranker), maxRank),
          heldBars_(heldBars), direction_(direction), excludeEntryExit_(excludeEntryExit)
    {
        if (heldBars == 0)
            throw std::invalid_argument("Holding period must be positive");
    }

    virtual bool qualifies(const Coin&, const BarData&, const MarketData&, Timestamp,
                           const IndicatorEngine&) const = 0;

    mutable std::unordered_map<Coin, double> entryStops_;
    mutable std::unordered_map<Coin, double> protectiveStops_;

private:
    unsigned int heldBars_;
    double direction_;
    bool excludeEntryExit_;
    mutable std::unordered_map<Coin, TradeRecord> held_;
};

class BargainChaserStrategy final : public TimedResearchStrategy {
public:
    BargainChaserStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
        std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int heldBars,
        double fallPercent, unsigned int averageLength)
        : TimedResearchStrategy("BargainChaser", maxSignals, std::move(universe), std::move(ranker), maxRank, heldBars),
          average_{IndicatorKind::SMA, PriceField::Close, averageLength}, fallPercent_(fallPercent) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(average_);
        specs.push_back(roc_);
        return specs;
    }

private:
    bool qualifies(const Coin& coin, const BarData& bar, const MarketData&, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const double average = indicators.value(coin, ts, average_);
        const double roc = indicators.value(coin, ts, roc_);
        return std::isfinite(average) && average > 0.0 && std::isfinite(roc) &&
            roc < -fallPercent_ / 100.0 && bar.close > average;
    }
    IndicatorSpec average_;
    IndicatorSpec roc_{IndicatorKind::ROC, PriceField::Close, 1};
    double fallPercent_;
};

class PureMomentumStrategy final : public TimedResearchStrategy {
public:
    PureMomentumStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
        std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int heldBars,
        unsigned int benchmarkAverageLength, std::string benchmark = "BTC")
        : TimedResearchStrategy("Pure_Mom", maxSignals, std::move(universe), std::move(ranker), maxRank, heldBars),
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, benchmarkAverageLength},
          benchmark_(std::move(benchmark)) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(benchmarkAverage_);
        return specs;
    }

private:
    bool qualifies(const Coin&, const BarData&, const MarketData& market, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const auto benchmarkBar = market.at(ts).find(benchmark_);
        const double average = indicators.value(benchmark_, ts, benchmarkAverage_);
        // Momentum ranks candidates; the original source has no positive-ROC filter.
        return benchmarkBar != market.at(ts).end() && std::isfinite(average) && average > 0.0 && benchmarkBar->second.close > average;
    }
    IndicatorSpec benchmarkAverage_;
    std::string benchmark_;
};

class RSIMeanReversionStrategy final : public TimedResearchStrategy {
public:
    RSIMeanReversionStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
        std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int rsiLength,
        double entryLevel, unsigned int heldBars)
        : TimedResearchStrategy("MR_RSILong", maxSignals, std::move(universe), std::move(ranker), maxRank, heldBars),
          rsi_{IndicatorKind::RSI, PriceField::Close, rsiLength}, entryLevel_(entryLevel) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(rsi_);
        return specs;
    }

private:
    bool qualifies(const Coin& coin, const BarData&, const MarketData&, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const double rsi = indicators.value(coin, ts, rsi_);
        // The original implementation has no BTC filter despite its old comment.
        return std::isfinite(rsi) && rsi < entryLevel_;
    }
    IndicatorSpec rsi_;
    double entryLevel_;
};

class ATRBreakoutStrategy final : public TimedResearchStrategy {
public:
    ATRBreakoutStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
        std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int heldBars,
        double atrMultiple, unsigned int atrLength)
        : TimedResearchStrategy("ATR_Breakout", maxSignals, std::move(universe), std::move(ranker), maxRank, heldBars, 1.0, true),
          atr_{IndicatorKind::ATR, PriceField::Close, atrLength}, atrMultiple_(atrMultiple) {}

    bool requiresStopEntries() const override { return true; }
    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(atr_);
        return specs;
    }

private:
    bool qualifies(const Coin& coin, const BarData& bar, const MarketData&, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const double atr = indicators.value(coin, ts, atr_);
        if (!std::isfinite(atr) || atr <= 0.0)
            return false;
        const double trigger = bar.high + atrMultiple_ * atr;
        if (!std::isfinite(trigger) || trigger <= 0.0)
            return false;
        entryStops_[coin] = trigger;
        return true;
    }
    IndicatorSpec atr_;
    double atrMultiple_;
};

class ShortMeanReversionStrategy final : public TimedResearchStrategy {
public:
    ShortMeanReversionStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
        std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int rsiLength,
        double rsiEntry, unsigned int benchmarkAverageLength, double atrMultiple,
        unsigned int atrLength, unsigned int heldBars, std::string benchmark = "BTC")
        : TimedResearchStrategy("MRShort", maxSignals, std::move(universe), std::move(ranker), maxRank, heldBars, -1.0, true),
          rsi_{IndicatorKind::RSI, PriceField::Close, rsiLength},
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, benchmarkAverageLength},
          atr_{IndicatorKind::ATR, PriceField::Close, atrLength}, rsiEntry_(rsiEntry), atrMultiple_(atrMultiple),
          benchmark_(std::move(benchmark)) {}

    bool requiresStopEntries() const override { return true; }
    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), {rsi_, benchmarkAverage_, atr_});
        return specs;
    }

private:
    bool qualifies(const Coin& coin, const BarData& bar, const MarketData& market, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        if (coin == "^GSPC")
            return false;
        const auto benchmarkBar = market.at(ts).find(benchmark_);
        const double average = indicators.value(benchmark_, ts, benchmarkAverage_);
        const double atr = indicators.value(coin, ts, atr_);
        const double rsi = indicators.value(coin, ts, rsi_);
        if (benchmarkBar == market.at(ts).end() || !std::isfinite(average) || average <= 0.0 ||
            benchmarkBar->second.close >= average || !std::isfinite(atr) || atr <= 0.0 ||
            !std::isfinite(rsi) || rsi <= rsiEntry_)
            return false;
        const double trigger = bar.close - atrMultiple_ * atr;
        if (!std::isfinite(trigger) || trigger <= 0.0)
            return false;
        entryStops_[coin] = trigger;
        // Preserve source behavior: signal CLOSE + ATR, despite its old "high" comment.
        protectiveStops_[coin] = bar.close + atr;
        return true;
    }
    IndicatorSpec rsi_;
    IndicatorSpec benchmarkAverage_;
    IndicatorSpec atr_;
    double rsiEntry_;
    double atrMultiple_;
    std::string benchmark_;
};
