#pragma once

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

#include "data_types.h"
#include "indicator_engine.h"
#include "indicator_spec.h"
#include "logger.h"
#include "market_filter.h"
#include "ranker.h"
#include "strategy.h"
#include "universe_selector.h"

/*
 * Legacy research-only PureRSI.
 *
 * This class intentionally targets the pre-SignalState Strategy API used by
 * the historical research executables.  The production/current PureRSI stays
 * in lib/src/strategy/strategies/pureRSI.h and is not modified.
 *
 * Entry signal : RSI > rsiEntry
 * Entry price  : next available bar OPEN
 * Exit signal  : RSI < rsiExit
 * Exit price   : next available bar OPEN
 * Sizing       : strategy_allocation * riskPerTrade_
 */
class StrategyPureRSI final : public Strategy {
public:
    StrategyPureRSI(
        unsigned int maxPosOpen,
        double qtyFraction,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int rsiLength,
        double rsiEntry,
        double rsiExit,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "Pure_RSI",
              maxPosOpen,
              qtyFraction,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition,
              std::move(marketFilters)
          ),
          rsiEntry_(rsiEntry),
          rsiExit_(rsiExit),
          rsiSpec_{IndicatorKind::RSI, PriceField::Close, rsiLength}
    {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();
        specs.push_back(rsiSpec_);
        return specs;
    }

protected:
    bool shouldEnter(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        const std::vector<Trade>&,
        double,
        const IndicatorEngine& indicators
    ) const override
    {
        const BarData* bar = findBar(marketData, coin, ts);
        const double rsi = indicators.value(coin, ts, rsiSpec_);

        return
            bar &&
            bar->close > 0.0 &&
            std::isfinite(rsi) &&
            rsi > rsiEntry_;
    }

    Trade buildTrade(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine&
    ) const override
    {
        (void)live_trading;

        Timestamp entryTs = 0;
        BarData entryBar;

        if (!findNextBar(marketData, coin, ts, entryTs, entryBar)) {
            LG_ERROR(
                "PureRSI: no next bar for {} at {}, using current close",
                coin,
                ts
            );

            const BarData* currentBar = findBar(marketData, coin, ts);
            if (!currentBar || currentBar->close <= 0.0)
                return invalidTrade();

            entryTs = ts;
            entryBar = *currentBar;
            entryBar.open = currentBar->close;
        }

        const double entryPrice =
            entryBar.open > 0.0 ? entryBar.open : entryBar.close;

        if (!std::isfinite(entryPrice) || entryPrice <= 0.0)
            return invalidTrade();

        const double tradeValue = strategy_allocation * riskPerTrade_;
        const double size = tradeValue / entryPrice;

        if (!std::isfinite(size) || size <= 0.0)
            return invalidTrade();

        Trade trade;
        trade.trade_id_ = ++last_trade_id;
        trade.start_ = entryTs;
        trade.end_ = 0;
        trade.coin_ = coin;
        trade.direction_ = Direction::Long;
        trade.entry_ = entryPrice;
        trade.exit_ = 0.0;
        trade.current_price_ = entryPrice;
        trade.size_ = size;
        trade.pnl_ = 0.0;
        trade.commission_ =
            entryPrice * size * commissionEntryFactor_;
        trade.sl_ = 0.0;
        trade.slReference_ = 0.0;
        trade.isSimulated_ = false;
        trade.exited_ = false;
        trade.barsHeld = 0;
        trade.strategy_name_ = strategy_name_;
        return trade;
    }

    void onBar(
        Trade& trade,
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const override
    {
        (void)live_trading;

        const BarData* bar = findBar(marketData, coin, ts);
        if (!bar || ts < trade.start_)
            return;

        trade.current_price_ = bar->close;
        trade.pnl_ =
            (trade.current_price_ - trade.entry_) * trade.size_;
        ++trade.barsHeld;

        const double rsi = indicators.value(coin, ts, rsiSpec_);
        if (!std::isfinite(rsi) || rsi >= rsiExit_)
            return;

        Timestamp exitTs = 0;
        BarData exitBar;

        if (findNextBar(marketData, coin, ts, exitTs, exitBar)) {
            trade.exit_ =
                exitBar.open > 0.0 ? exitBar.open : exitBar.close;
            trade.end_ = exitTs;
        } else {
            trade.exit_ = bar->close;
            trade.end_ = ts;
        }

        trade.exited_ = true;
        trade.current_price_ = trade.exit_;
        trade.pnl_ =
            (trade.exit_ - trade.entry_) * trade.size_;
        trade.commission_ +=
            trade.exit_ * trade.size_ * commissionExitFactor_;
    }

private:
    const BarData* findBar(
        const MarketData& marketData,
        const Coin& coin,
        Timestamp ts
    ) const
    {
        const auto tsIt = marketData.find(ts);
        if (tsIt == marketData.end())
            return nullptr;

        const auto coinIt = tsIt->second.find(coin);
        if (coinIt == tsIt->second.end())
            return nullptr;

        return &coinIt->second;
    }

    bool findNextBar(
        const MarketData& marketData,
        const Coin& coin,
        Timestamp ts,
        Timestamp& nextTs,
        BarData& nextBar
    ) const
    {
        auto it = marketData.upper_bound(ts);
        while (it != marketData.end()) {
            const auto coinIt = it->second.find(coin);
            if (coinIt != it->second.end()) {
                nextTs = it->first;
                nextBar = coinIt->second;
                return true;
            }
            ++it;
        }
        return false;
    }

    Trade invalidTrade() const
    {
        Trade trade;
        trade.strategy_name_ = strategy_name_;
        trade.isSimulated_ = true;
        trade.exited_ = true;
        return trade;
    }

private:
    double rsiEntry_ = 80.0;
    double rsiExit_ = 70.0;
    IndicatorSpec rsiSpec_;
};
