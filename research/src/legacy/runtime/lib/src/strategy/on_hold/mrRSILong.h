#pragma once

#include <cmath>
#include <memory>
#include <string>
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


/**************************************************************************************
 * Type    : StrategyMRRSILong
 * Purpose : Long mean-reversion strategy that buys oversold RSI names
 *
 * RealTest equivalent:
 *
 *   Strategy: MR_RSILong
 *   Side: Long
 *   QtyType: Percent
 *   Quantity: QtyPct
 *   SetupScore: ROC(C, MOMn)
 *   MaxPositions: MaxPos
 *
 *   EntrySetup:
 *       CanTrade
 *       RSI(RSIL) < RSIn
 *       Extern($BTCUSDT, C > MA(C, BTCMALen))
 *
 *   ExitRule:
 *       BarsHeld == BarsH
 *
 * Notes:
 *   - Entry is executed at next bar open after setup.
 *   - Exit is executed at next bar open after BarsH is reached.
 **************************************************************************************/
class StrategyMRRSILong final : public Strategy {
public:
    StrategyMRRSILong(
        unsigned int maxPosOpen,
        double qtyFraction,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int rsiLength,
        double rsiEntryLevel,
        unsigned int heldBars,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "MR_RSILong",
              maxPosOpen,
              qtyFraction,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition,
              std::move(marketFilters)
          ),
          rsiEntryLevel_(rsiEntryLevel),
          heldBars_(heldBars),
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
            rsi < rsiEntryLevel_ ;
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
                "Could not find next day for coin {} and date {} when building the trade, using current close instead",
                coin,
                ts
            );

            const BarData* currentBar = findBar(marketData, coin, ts);

            if (!currentBar || currentBar->close <= 0.0) {
                return invalidTrade();
            }

            entryTs = ts;
            entryBar = *currentBar;
            entryBar.open = currentBar->close;
        }

        const double entryPrice =
            entryBar.open > 0.0 ? entryBar.open : entryBar.close;

        if (entryPrice <= 0.0) {
            return invalidTrade();
        }

        const double tradeValue = strategy_allocation * riskPerTrade_;
        const double size = tradeValue / entryPrice;

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
        const IndicatorEngine&
    ) const override
    {
        (void)live_trading;

        const BarData* bar = findBar(marketData, coin, ts);

        if (!bar || ts < trade.start_) {
            return;
        }

        trade.current_price_ = bar->close;
        trade.pnl_ = (trade.current_price_ - trade.entry_) * trade.size_;

        ++trade.barsHeld;

        if (trade.barsHeld < heldBars_) {
            return;
        }

        Timestamp exitTs = 0;
        BarData exitBar;

        if (findNextBar(marketData, coin, ts, exitTs, exitBar)) {
            trade.exit_ =
                exitBar.open > 0.0 ? exitBar.open : exitBar.close;

            trade.end_ = exitTs;
        } else {
            LG_ERROR(
                "Could not find next day for coin {} and date {} when exiting the trade, using close instead of next open",
                coin,
                ts
            );

            trade.exit_ = bar->close;
            trade.end_ = ts;
        }

        trade.exited_ = true;

        trade.pnl_ = (trade.exit_ - trade.entry_) * trade.size_;

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

        if (tsIt == marketData.end()) {
            return nullptr;
        }

        const auto coinIt = tsIt->second.find(coin);

        if (coinIt == tsIt->second.end()) {
            return nullptr;
        }

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
    double rsiEntryLevel_ = 0.0;
    unsigned int heldBars_ = 0;

    IndicatorSpec rsiSpec_;
};