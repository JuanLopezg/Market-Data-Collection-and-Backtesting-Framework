#pragma once

#include "indicator_engine.h"
#include "strategy.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

// Research-only stop-entry experiments. Signals describe desired exposure;
// CURRENT sizing, order lifecycle, simulated matching and accounting own money.
class XHBreakoutStrategy final : public Strategy {
public:
    XHBreakoutStrategy(
        unsigned int maxSignals, std::unique_ptr<UniverseSelector> universe,
        std::unique_ptr<Ranker> ranker, unsigned int maxRank,
        unsigned int lookback, unsigned int exitLength, double atrMultiplier = 0.0
    )
        : Strategy(atrMultiplier > 0 ? "xH_Breakout_ATR" : "xH_Breakout", maxSignals,
                   std::move(universe), std::move(ranker), maxRank),
          highest_{IndicatorKind::Highest, PriceField::High, lookback},
          exitIndicator_{atrMultiplier > 0 ? IndicatorKind::ATR : IndicatorKind::SMA,
                         PriceField::Close, exitLength},
          atrMultiplier_(atrMultiplier)
    {
        if (!std::isfinite(atrMultiplier) || atrMultiplier < 0.0 || lookback == 0 || exitLength == 0)
            throw std::invalid_argument("Invalid XH breakout parameters");
    }

    bool requiresStopEntries() const override { return true; }

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        auto specs = Strategy::requiredIndicators();
        specs.push_back(highest_);
        specs.push_back(exitIndicator_);
        return specs;
    }

    void observeTrades(const std::vector<TradeRecord>& trades) const override
    {
        held_.clear();
        for (const auto& trade : trades) {
            if (!trade.exited)
                held_.emplace(trade.coin, trade);
        }
    }

    double entryStopPrice(const Coin& coin) const override
    {
        return stopPrices_.at(coin);
    }

    void updateSignals(const MarketData& market, Timestamp ts, SignalState& signals,
                       const IndicatorEngine& indicators) const override
    {
        const auto slice = market.find(ts);
        if (slice == market.end())
            return;
        // Unfilled one-bar candidates have expired before this close. Start from
        // actual held campaigns, not yesterday's unfilled signal candidates.
        std::vector<Coin> oldSignals;
        for (const auto& [coin, value] : signals.values()) {
            (void)value;
            oldSignals.push_back(coin);
        }
        for (const auto& coin : oldSignals)
            signals.set(coin, 0);
        stopPrices_.clear();
        for (const auto& [coin, trade] : held_) {
            const auto bar = slice->second.find(coin);
            bool exit = false;
            if (bar != slice->second.end()) {
                if (atrMultiplier_ > 0) {
                    const double atr = indicators.value(coin, ts, exitIndicator_);
                    if (ts > trade.start && std::isfinite(atr) && atr > 0) {
                        const double stop = trailingStop(market, ts, trade, indicators);
                        exit = stop > 0 && bar->second.close < stop;
                    }
                } else {
                    const double average = indicators.value(coin, ts, exitIndicator_);
                    exit = std::isfinite(average) && average > 0 && bar->second.close < average;
                }
            }
            if (!exit)
                signals.set(coin, 1);
        }
        if (!marketFiltersPass(market, ts, indicators))
            return;
        const auto tradable = universeSelector_->select(slice->second, ts, indicators);
        unsigned int rank = 0;
        // Pending entries reserve slots together with held positions, including
        // positions whose exit has been signaled but has not filled yet.
        std::size_t reserved = held_.size();
        for (const auto& candidate : ranker_->rank(tradable, ts, indicators)) {
            if (++rank > maxRankingPosition_ || reserved >= maxActiveSignals_)
                break;
            if (held_.contains(candidate.coin))
                continue;
            const double trigger = indicators.value(candidate.coin, ts, highest_);
            const double exitValue = indicators.value(candidate.coin, ts, exitIndicator_);
            if (!std::isfinite(trigger) || trigger <= 0 ||
                (atrMultiplier_ > 0 && (!std::isfinite(exitValue) || exitValue <= 0)))
                continue;
            stopPrices_.emplace(candidate.coin, trigger);
            signals.set(candidate.coin, 1);
            ++reserved;
        }
    }

private:
    double trailingStop(const MarketData& market, Timestamp ts, const TradeRecord& trade,
                        const IndicatorEngine& indicators) const
    {
        // Rebuild the ratchet from actual entry metadata and completed history.
        // This avoids an independent, non-recoverable trailing-state cache.
        const double initialAtr = indicators.value(trade.coin, trade.start, exitIndicator_);
        double high = trade.entry_price;
        double stop = std::isfinite(initialAtr) && initialAtr > 0
            ? std::max(0.0, high - atrMultiplier_ * initialAtr) : 0.0;
        // Exclude entry-bar high: OHLC cannot establish its ordering relative to fill.
        for (auto day = market.upper_bound(trade.start); day != market.end() && day->first <= ts; ++day) {
            const auto bar = day->second.find(trade.coin);
            const double atr = indicators.value(trade.coin, day->first, exitIndicator_);
            if (bar == day->second.end() || !std::isfinite(atr) || atr <= 0)
                continue;
            high = std::max(high, bar->second.high);
            stop = std::max(stop, high - atrMultiplier_ * atr);
        }
        return stop;
    }

    IndicatorSpec highest_;
    IndicatorSpec exitIndicator_;
    double atrMultiplier_;
    mutable std::unordered_map<Coin, TradeRecord> held_;
    mutable std::unordered_map<Coin, double> stopPrices_;
};
