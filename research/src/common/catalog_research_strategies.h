#pragma once

#include "indicator_engine.h"
#include "strategy.h"
#include "timed_research_strategies.h"

#include <cmath>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Research-only strategies of the user's RealTest catalog (2026-10-22) that had no current
// implementation: the first ("plain") variant of each. Signals at close(T), fills at the
// next open (market), at the next bar's stop or limit (entry stop/limit). Not validated for
// live trading.

// ---------------------------------------------------------------- condition strategies
// Persistent signals like PureRSI: an active signal exits when exits() says so; new ones
// enter by rank while entries() holds and there is room.
class ConditionResearchStrategy : public Strategy {
public:
    void updateSignals(const MarketData& market, Timestamp ts, SignalState& signals,
                       const IndicatorEngine& indicators) const override
    {
        const auto slice = market.find(ts);
        if (slice == market.end())
            return;
        prepare(market, ts);
        std::vector<Coin> exits;
        for (const auto& [coin, value] : signals.values()) {
            if (value == 0.0)
                continue;
            const auto bar = slice->second.find(coin);
            if (bar != slice->second.end() && exitsNow(coin, bar->second, market, ts, indicators))
                exits.push_back(coin);
        }
        for (const Coin& coin : exits)
            signals.set(coin, 0.0);
        if (signals.activeCount() >= maxActiveSignals_ || !marketFiltersPass(market, ts, indicators))
            return;
        const auto tradable = universeSelector_->select(slice->second, ts, indicators);
        unsigned int rank = 0;
        for (const auto& candidate : ranker_->rank(tradable, ts, indicators)) {
            if (++rank > maxRankingPosition_ || signals.activeCount() >= maxActiveSignals_)
                break;
            if (signals.isActive(candidate.coin))
                continue;
            const auto bar = slice->second.find(candidate.coin);
            if (bar != slice->second.end() && std::isfinite(bar->second.close) && bar->second.close > 0.0 &&
                entersNow(candidate.coin, bar->second, market, ts, indicators))
                signals.set(candidate.coin, 1.0);
        }
    }

protected:
    using Strategy::Strategy;
    virtual void prepare(const MarketData&, Timestamp) const {}
    virtual bool entersNow(const Coin&, const BarData&, const MarketData&, Timestamp, const IndicatorEngine&) const = 0;
    virtual bool exitsNow(const Coin&, const BarData&, const MarketData&, Timestamp, const IndicatorEngine&) const = 0;

    // Benchmark close above its SMA on ts (Extern($BTC, C > MA(C, n))).
    static bool benchmarkAbove(const std::string& benchmark, const IndicatorSpec& average, const MarketData& market,
                               Timestamp ts, const IndicatorEngine& indicators)
    {
        const auto bar = market.at(ts).find(benchmark);
        const double value = indicators.value(benchmark, ts, average);
        return bar != market.at(ts).end() && std::isfinite(value) && value > 0.0 && bar->second.close > value;
    }
};

// ATH_Chaser: enter when C > ATH x (1 - fromAth); exit when C < ATH x (1 - fromAth) x
// (1 - exitFromEntry). ATH = highest high since the asset's first bar (Highest(H, BarNum)).
class ATHChaserStrategy final : public ConditionResearchStrategy {
public:
    ATHChaserStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
                      std::unique_ptr<Ranker> ranker, unsigned int maxRank, double fromAth, double exitFromEntry)
        : ConditionResearchStrategy("ATH_Chaser", maxSignals, std::move(universe), std::move(ranker), maxRank),
          fromAth_(fromAth), exitFromEntry_(exitFromEntry) {}

private:
    // Running all-time high per asset up to ts, built incrementally (no future bar).
    void prepare(const MarketData& market, Timestamp ts) const override
    {
        auto day = processedThrough_ == 0 ? market.begin() : market.upper_bound(processedThrough_);
        for (; day != market.end() && day->first <= ts; ++day) {
            for (const auto& [coin, bar] : day->second)
                if (std::isfinite(bar.high))
                    ath_[coin] = std::max(ath_[coin], bar.high);
            processedThrough_ = day->first;
        }
    }
    double ath(const Coin& coin) const
    {
        const auto it = ath_.find(coin);
        return it == ath_.end() ? 0.0 : it->second;
    }
    bool entersNow(const Coin& coin, const BarData& bar, const MarketData&, Timestamp, const IndicatorEngine&) const override
    {
        return ath(coin) > 0.0 && bar.close > ath(coin) * (1.0 - fromAth_);
    }
    bool exitsNow(const Coin& coin, const BarData& bar, const MarketData&, Timestamp, const IndicatorEngine&) const override
    {
        return ath(coin) > 0.0 && bar.close < ath(coin) * (1.0 - fromAth_) * (1.0 - exitFromEntry_);
    }
    double fromAth_;
    double exitFromEntry_;
    mutable Timestamp processedThrough_ = 0;
    mutable std::unordered_map<Coin, double> ath_;
};

// MA_Cross: enter when MA(fast) > MA(slow) and benchmark > MA(btcLength); exit when
// MA(fast) < MA(slow).
class MACrossStrategy final : public ConditionResearchStrategy {
public:
    MACrossStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe, std::unique_ptr<Ranker> ranker,
                    unsigned int maxRank, unsigned int fastLength, unsigned int slowLength,
                    unsigned int btcLength, std::string benchmark)
        : ConditionResearchStrategy("MA_Cross", maxSignals, std::move(universe), std::move(ranker), maxRank),
          fast_{IndicatorKind::SMA, PriceField::Close, fastLength}, slow_{IndicatorKind::SMA, PriceField::Close, slowLength},
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, btcLength}, benchmark_(std::move(benchmark)) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), {fast_, slow_, benchmarkAverage_});
        return specs;
    }

private:
    bool entersNow(const Coin& coin, const BarData&, const MarketData& market, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const double fast = indicators.value(coin, ts, fast_), slow = indicators.value(coin, ts, slow_);
        return std::isfinite(fast) && std::isfinite(slow) && fast > slow &&
            benchmarkAbove(benchmark_, benchmarkAverage_, market, ts, indicators);
    }
    bool exitsNow(const Coin& coin, const BarData&, const MarketData&, Timestamp ts,
                  const IndicatorEngine& indicators) const override
    {
        const double fast = indicators.value(coin, ts, fast_), slow = indicators.value(coin, ts, slow_);
        return std::isfinite(fast) && std::isfinite(slow) && fast < slow;
    }
    IndicatorSpec fast_, slow_, benchmarkAverage_;
    std::string benchmark_;
};

// MA_Simple: enter when C > MA(length) and benchmark > MA(btcLength); exit when C < MA(length).
class MASimpleStrategy final : public ConditionResearchStrategy {
public:
    MASimpleStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe, std::unique_ptr<Ranker> ranker,
                     unsigned int maxRank, unsigned int length, unsigned int btcLength, std::string benchmark)
        : ConditionResearchStrategy("MA_Simple", maxSignals, std::move(universe), std::move(ranker), maxRank),
          average_{IndicatorKind::SMA, PriceField::Close, length},
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, btcLength}, benchmark_(std::move(benchmark)) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), {average_, benchmarkAverage_});
        return specs;
    }

private:
    bool entersNow(const Coin& coin, const BarData& bar, const MarketData& market, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const double average = indicators.value(coin, ts, average_);
        return std::isfinite(average) && bar.close > average &&
            benchmarkAbove(benchmark_, benchmarkAverage_, market, ts, indicators);
    }
    bool exitsNow(const Coin& coin, const BarData& bar, const MarketData&, Timestamp ts,
                  const IndicatorEngine& indicators) const override
    {
        const double average = indicators.value(coin, ts, average_);
        return std::isfinite(average) && bar.close < average;
    }
    IndicatorSpec average_, benchmarkAverage_;
    std::string benchmark_;
};

// Follow_BTC: hold the top-liquidity assets while the benchmark closes above MA(length);
// every signal exits when it closes below.
class FollowBenchmarkStrategy final : public ConditionResearchStrategy {
public:
    FollowBenchmarkStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
                            std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int length, std::string benchmark)
        : ConditionResearchStrategy("Follow_BTC", maxSignals, std::move(universe), std::move(ranker), maxRank),
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, length}, benchmark_(std::move(benchmark)) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(benchmarkAverage_);
        return specs;
    }

private:
    bool entersNow(const Coin&, const BarData&, const MarketData& market, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        return benchmarkAbove(benchmark_, benchmarkAverage_, market, ts, indicators);
    }
    bool exitsNow(const Coin&, const BarData&, const MarketData& market, Timestamp ts,
                  const IndicatorEngine& indicators) const override
    {
        const auto bar = market.at(ts).find(benchmark_);
        const double value = indicators.value(benchmark_, ts, benchmarkAverage_);
        return bar != market.at(ts).end() && std::isfinite(value) && bar->second.close < value;
    }
    IndicatorSpec benchmarkAverage_;
    std::string benchmark_;
};

// ---------------------------------------------------------------- timed strategies
// Entry at the next bar's stop or limit, exit after heldBars (BarsHeld == n).

// xH_Breakout (first variant): benchmark > MA(btcLength), stop entry at Highest(H, xH),
// exit after heldBars.
class XHBreakoutTimedStrategy final : public TimedResearchStrategy {
public:
    XHBreakoutTimedStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
                            std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int lookback,
                            unsigned int heldBars, unsigned int btcLength, std::string benchmark)
        : TimedResearchStrategy("xH_Breakout_Timed", maxSignals, std::move(universe), std::move(ranker), maxRank,
                                heldBars, 1.0, true),
          highest_{IndicatorKind::Highest, PriceField::High, lookback},
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, btcLength}, benchmark_(std::move(benchmark)) {}

    bool requiresStopEntries() const override { return true; }
    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), {highest_, benchmarkAverage_});
        return specs;
    }

private:
    bool qualifies(const Coin& coin, const BarData&, const MarketData& market, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const auto benchmarkBar = market.at(ts).find(benchmark_);
        const double average = indicators.value(benchmark_, ts, benchmarkAverage_);
        const double trigger = indicators.value(coin, ts, highest_);
        if (benchmarkBar == market.at(ts).end() || !std::isfinite(average) || benchmarkBar->second.close <= average ||
            !std::isfinite(trigger) || trigger <= 0.0)
            return false;
        entryStops_[coin] = trigger;
        return true;
    }
    IndicatorSpec highest_, benchmarkAverage_;
    std::string benchmark_;
};

// Long limit-entry mean reversion with a benchmark uptrend filter: the limit price comes
// from limitFor(); exit after heldBars.
class LimitEntryResearchStrategy : public TimedResearchStrategy {
public:
    bool requiresStopEntries() const override { return true; }
    bool entryIsLimit() const override { return true; }

protected:
    LimitEntryResearchStrategy(std::string name, unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
                               std::unique_ptr<Ranker> ranker, unsigned int maxRank, unsigned int heldBars,
                               unsigned int btcLength, std::string benchmark)
        : TimedResearchStrategy(std::move(name), maxSignals, std::move(universe), std::move(ranker), maxRank,
                                heldBars, 1.0, true),
          benchmarkAverage_{IndicatorKind::SMA, PriceField::Close, btcLength}, benchmark_(std::move(benchmark)) {}

    virtual double limitFor(const Coin&, const BarData&, Timestamp, const IndicatorEngine&) const = 0;
    IndicatorSpec benchmarkAverage_;

private:
    bool qualifies(const Coin& coin, const BarData& bar, const MarketData& market, Timestamp ts,
                   const IndicatorEngine& indicators) const override
    {
        const auto benchmarkBar = market.at(ts).find(benchmark_);
        const double average = indicators.value(benchmark_, ts, benchmarkAverage_);
        if (benchmarkBar == market.at(ts).end() || !std::isfinite(average) || benchmarkBar->second.close <= average)
            return false;
        const double limit = limitFor(coin, bar, ts, indicators);
        if (!std::isfinite(limit) || limit <= 0.0)
            return false;
        entryStops_[coin] = limit;
        return true;
    }
    std::string benchmark_;
};

// MR_LowsSweeper: limit at Lowest(L, xL), exit after heldBars.
class LowsSweeperStrategy final : public LimitEntryResearchStrategy {
public:
    LowsSweeperStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe, std::unique_ptr<Ranker> ranker,
                        unsigned int maxRank, unsigned int lookback, unsigned int heldBars, unsigned int btcLength,
                        std::string benchmark)
        : LimitEntryResearchStrategy("MR_LowsSweeper", maxSignals, std::move(universe), std::move(ranker), maxRank,
                                     heldBars, btcLength, std::move(benchmark)),
          lowest_{IndicatorKind::Lowest, PriceField::Low, lookback} {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), {lowest_, benchmarkAverage_});
        return specs;
    }

private:
    double limitFor(const Coin& coin, const BarData&, Timestamp ts, const IndicatorEngine& indicators) const override
    {
        return indicators.value(coin, ts, lowest_);
    }
    IndicatorSpec lowest_;
};

// MR_ATRLimit: limit at C - atrMultiple x ATR(atrLength), exit after heldBars.
class ATRLimitStrategy final : public LimitEntryResearchStrategy {
public:
    ATRLimitStrategy(unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe, std::unique_ptr<Ranker> ranker,
                     unsigned int maxRank, unsigned int atrLength, double atrMultiple, unsigned int heldBars,
                     unsigned int btcLength, std::string benchmark)
        : LimitEntryResearchStrategy("MR_ATRLimit", maxSignals, std::move(universe), std::move(ranker), maxRank,
                                     heldBars, btcLength, std::move(benchmark)),
          atr_{IndicatorKind::ATR, PriceField::Close, atrLength}, atrMultiple_(atrMultiple) {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.insert(specs.end(), {atr_, benchmarkAverage_});
        return specs;
    }

private:
    double limitFor(const Coin& coin, const BarData& bar, Timestamp ts, const IndicatorEngine& indicators) const override
    {
        const double atr = indicators.value(coin, ts, atr_);
        return std::isfinite(atr) && atr > 0.0 ? bar.close - atrMultiple_ * atr : 0.0;
    }
    IndicatorSpec atr_;
    double atrMultiple_;
};
