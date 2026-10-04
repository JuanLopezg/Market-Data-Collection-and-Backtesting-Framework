#pragma once

#include <algorithm>
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


/**************************************************************************************
 * Strategy : StrategyXHBreakout_ATR
 *
 * Entry:
 *   Creates a one-bar stop-entry order at:
 *
 *     Highest(High, highestLength)
 *
 * Exit:
 *   Maintains the highest high observed since the trade entered and calculates:
 *
 *     ATR stop = highestHighSinceEntry - atrMultiplier * ATR(atrLength)
 *
 *   The stop is ratcheted upward and is never allowed to move downward.
 *
 *   An exit signal occurs when:
 *
 *     current close < ATR stop
 *
 *   The trade is then closed at the next available bar open. If no later bar exists,
 *   it is closed at the current bar close.
 *
 * Trade fields:
 *   trade.slReference_ = highest high observed since entry
 *   trade.sl_          = current effective ATR trailing stop
 **************************************************************************************/
class StrategyXHBreakout_ATR final : public Strategy {
public:
    StrategyXHBreakout_ATR(
        unsigned int maxPosOpen,
        double qtyFraction,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int highestLength,
        unsigned int atrLength,
        double atrMultiplier,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "xH_Breakout_ATR",
              maxPosOpen,
              qtyFraction,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition,
              std::move(marketFilters)
          ),
          entryHighestSpec_{
              IndicatorKind::Highest,
              PriceField::High,
              highestLength
          },
          atrSpec_{
              IndicatorKind::ATR,
              PriceField::Close,
              atrLength
          },
          atrMultiplier_(atrMultiplier)
    {}


    /**********************************************************************************
     * Purpose : Declare all indicators needed by this strategy
     **********************************************************************************/
    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();

        specs.push_back(entryHighestSpec_);
        specs.push_back(atrSpec_);

        return specs;
    }


protected:
    /**********************************************************************************
     * Purpose : This strategy enters through stop-entry orders
     **********************************************************************************/
    bool usesEntryOrders() const override
    {
        return true;
    }


    /**********************************************************************************
     * Purpose : Check whether a valid breakout level exists
     *
     * The actual breakout is handled by the stop order on the next bar.
     **********************************************************************************/
    bool shouldEnter(
        const Coin& coin,
        const MarketData&,
        Timestamp ts,
        const std::vector<Trade>&,
        double,
        const IndicatorEngine& indicators
    ) const override
    {
        const double entryStop =
            indicators.value(coin, ts, entryHighestSpec_);

        return std::isfinite(entryStop) &&
               entryStop > 0.0;
    }


    /**********************************************************************************
     * Purpose : Build a one-bar stop-entry order at the rolling highest high
     **********************************************************************************/
    Order buildOrder(
        const Coin& coin,
        const MarketData&,
        Timestamp ts,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const override
    {
        (void)live_trading;

        Order order;

        order.created_ = ts;
        order.active_from_ = ts;
        order.coin_ = coin;
        order.direction_ = Direction::Long;
        order.type_ = OrderType::Stop;
        order.status_ = OrderStatus::Pending;
        order.tif_ = OrderTimeInForce::GoodForBars;
        order.barsValid_ = 1;
        order.barsAlive_ = 0;
        order.strategy_name_ = strategy_name_;

        const double entryStop =
            indicators.value(coin, ts, entryHighestSpec_);

        if (!std::isfinite(entryStop) || entryStop <= 0.0) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        const double tradeValue =
            strategy_allocation * riskPerTrade_;

        if (!std::isfinite(tradeValue) || tradeValue <= 0.0) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        order.trigger_price_ = entryStop;
        order.size_ = tradeValue / entryStop;

        /*
         * The ATR stop is initialized only after the order fills, because its
         * starting reference must be based on the actual fill price.
         */
        order.sl_ = 0.0;
        order.slReference_ = entryStop;

        return order;
    }


    /**********************************************************************************
     * Purpose : Check whether a pending long stop order fills on this bar
     **********************************************************************************/
    bool shouldFillOrder(
        const Order& order,
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine&
    ) const override
    {
        if (order.strategy_name_ != strategy_name_) {
            return false;
        }

        if (order.status_ != OrderStatus::Pending) {
            return false;
        }

        if (order.type_ != OrderType::Stop) {
            return false;
        }

        if (order.direction_ != Direction::Long) {
            return false;
        }

        /*
         * An order cannot fill on the bar on which it was created.
         */
        if (ts <= order.created_) {
            return false;
        }

        if (!std::isfinite(order.trigger_price_) ||
            order.trigger_price_ <= 0.0) {
            return false;
        }

        const BarData* bar =
            findBar(marketData, order.coin_, ts);

        if (!bar) {
            return false;
        }

        return bar->high >= order.trigger_price_;
    }


    /**********************************************************************************
     * Purpose : Convert a filled stop-entry order into a real trade
     **********************************************************************************/
    Trade buildTradeFromOrder(
        const Order& order,
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const override
    {
        (void)live_trading;

        if (!shouldFillOrder(
                order,
                marketData,
                ts,
                indicators
            )) {
            return invalidTrade();
        }

        const BarData* bar =
            findBar(marketData, order.coin_, ts);

        if (!bar) {
            return invalidTrade();
        }

        /*
         * Stop-order fill model:
         *
         * - If the market opens below the stop and later trades through it,
         *   fill at the stop.
         *
         * - If the market gaps above the stop, fill at the bar open.
         */
        const double fillPrice =
            std::max(order.trigger_price_, bar->open);

        if (!std::isfinite(fillPrice) || fillPrice <= 0.0) {
            return invalidTrade();
        }

        /*
         * Preserve the intended monetary value of the original order even if
         * a gap causes the actual fill price to differ from the trigger.
         */
        const double intendedTradeValue =
            order.size_ * order.trigger_price_;

        const double finalSize =
            intendedTradeValue / fillPrice;

        if (!std::isfinite(finalSize) || finalSize <= 0.0) {
            return invalidTrade();
        }

        const double atr =
            indicators.value(order.coin_, ts, atrSpec_);

        if (!std::isfinite(atr) || atr <= 0.0) {
            return invalidTrade();
        }

        if (!std::isfinite(atrMultiplier_) ||
            atrMultiplier_ <= 0.0) {
            return invalidTrade();
        }

        Trade trade;

        trade.trade_id_ = ++last_trade_id;
        trade.source_order_id_ = order.order_id_;

        trade.start_ = ts;
        trade.end_ = 0;

        trade.coin_ = order.coin_;
        trade.direction_ = Direction::Long;

        trade.entry_ = fillPrice;
        trade.exit_ = 0.0;
        trade.current_price_ = bar->close;

        trade.size_ = finalSize;
        trade.pnl_ =
            (trade.current_price_ - trade.entry_) *
            trade.size_;

        trade.commission_ =
            trade.entry_ *
            trade.size_ *
            commissionEntryFactor_;

        /*
         * Because only OHLC bars are available, it is not possible to know
         * whether the entry bar's high happened before or after the stop order
         * filled. Starting from fillPrice avoids using unknown intrabar ordering.
         *
         * From the next bar onward, slReference_ is updated with each bar's high.
         */
        trade.slReference_ = fillPrice;

        const double initialStop =
            trade.slReference_ -
            atrMultiplier_ * atr;

        trade.sl_ =
            std::isfinite(initialStop) && initialStop > 0.0
                ? initialStop
                : 0.0;

        trade.isSimulated_ = false;
        trade.exited_ = false;
        trade.barsHeld = 1;
        trade.strategy_name_ = strategy_name_;

        return trade;
    }


    /**********************************************************************************
     * Purpose : Cancel an unfilled one-bar entry order
     **********************************************************************************/
    bool shouldCancelOrder(
        const Order& order,
        const MarketData&,
        Timestamp ts,
        const IndicatorEngine&
    ) const override
    {
        if (order.strategy_name_ != strategy_name_) {
            return false;
        }

        if (order.status_ != OrderStatus::Pending) {
            return true;
        }

        /*
         * Keep the order alive on its creation bar.
         */
        if (ts <= order.created_) {
            return false;
        }

        /*
         * If it did not fill on the next processed bar, cancel it.
         */
        return true;
    }


    /**********************************************************************************
     * Purpose : Update an open trade and apply the ATR trailing-stop exit
     *
     * Rule:
     *
     *   highestHighSinceEntry =
     *       max(previousHighestHighSinceEntry, currentBar.high)
     *
     *   rawATRStop =
     *       highestHighSinceEntry - atrMultiplier * ATR(atrLength)
     *
     *   effectiveStop =
     *       max(previousEffectiveStop, rawATRStop)
     *
     * Exit signal:
     *
     *   currentBar.close < effectiveStop
     **********************************************************************************/
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

        if (trade.exited_) {
            return;
        }

        /*
         * Do not evaluate the exit on the entry/fill bar.
         */
        if (ts <= trade.start_) {
            return;
        }

        const BarData* bar =
            findBar(marketData, coin, ts);

        if (!bar) {
            return;
        }

        trade.current_price_ = bar->close;

        trade.pnl_ =
            (trade.current_price_ - trade.entry_) *
            trade.size_;

        ++trade.barsHeld;

        const double atr =
            indicators.value(coin, ts, atrSpec_);

        if (!std::isfinite(atr) || atr <= 0.0) {
            return;
        }

        if (!std::isfinite(atrMultiplier_) ||
            atrMultiplier_ <= 0.0) {
            return;
        }

        /*
         * This is the dynamic equivalent of Highest(High, bars since entry).
         *
         * It is stored on the trade because each trade has a different entry
         * timestamp and therefore a different expanding lookback window.
         */
        trade.slReference_ =
            std::max(trade.slReference_, bar->high);

        const double rawATRStop =
            trade.slReference_ -
            atrMultiplier_ * atr;

        if (!std::isfinite(rawATRStop) ||
            rawATRStop <= 0.0) {
            return;
        }

        /*
         * Ratchet the stop upward. ATR may increase, which would otherwise
         * allow the formula to move the stop downward.
         */
        if (!std::isfinite(trade.sl_) || trade.sl_ <= 0.0) {
            trade.sl_ = rawATRStop;
        } else {
            trade.sl_ =
                std::max(trade.sl_, rawATRStop);
        }

        const bool exitSignal =
            bar->close < trade.sl_;

        if (!exitSignal) {
            return;
        }

        Timestamp nextTs = 0;
        BarData nextBar;

        if (findNextBar(
                marketData,
                coin,
                ts,
                nextTs,
                nextBar
            )) {
            closeTradeAtBarOpen(
                trade,
                nextBar,
                nextTs
            );
        } else {
            closeTradeAtBarClose(
                trade,
                *bar,
                ts
            );
        }
    }


    /**********************************************************************************
     * Purpose : Prevent direct market-entry trade construction
     **********************************************************************************/
    Trade buildTrade(
        const Coin&,
        const MarketData&,
        Timestamp,
        unsigned int&,
        double,
        bool,
        const IndicatorEngine&
    ) const override
    {
        LG_ERROR(
            "Strategy {} should not call buildTrade() directly because it uses entry orders",
            strategy_name_
        );

        return invalidTrade();
    }


private:
    /**********************************************************************************
     * Purpose : Find one coin's bar at a timestamp
     **********************************************************************************/
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


    /**********************************************************************************
     * Purpose : Find the next available bar for a coin
     **********************************************************************************/
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


    /**********************************************************************************
     * Purpose : Close a long trade at the next bar's open
     **********************************************************************************/
    void closeTradeAtBarOpen(
        Trade& trade,
        const BarData& bar,
        Timestamp ts
    ) const
    {
        trade.exit_ = bar.open;
        trade.end_ = ts;
        trade.exited_ = true;
        trade.current_price_ = bar.open;

        trade.pnl_ =
            (trade.exit_ - trade.entry_) *
            trade.size_;

        trade.commission_ +=
            trade.exit_ *
            trade.size_ *
            commissionExitFactor_;
    }


    /**********************************************************************************
     * Purpose : Close a long trade at the current bar's close
     **********************************************************************************/
    void closeTradeAtBarClose(
        Trade& trade,
        const BarData& bar,
        Timestamp ts
    ) const
    {
        trade.exit_ = bar.close;
        trade.end_ = ts;
        trade.exited_ = true;
        trade.current_price_ = bar.close;

        trade.pnl_ =
            (trade.exit_ - trade.entry_) *
            trade.size_;

        trade.commission_ +=
            trade.exit_ *
            trade.size_ *
            commissionExitFactor_;
    }


    /**********************************************************************************
     * Purpose : Return a rejected/non-real trade
     **********************************************************************************/
    Trade invalidTrade() const
    {
        Trade trade;

        trade.strategy_name_ = strategy_name_;
        trade.isSimulated_ = true;
        trade.exited_ = true;

        return trade;
    }


private:
    /*
     * Fixed rolling Highest(High, highestLength) used for breakout entries.
     */
    IndicatorSpec entryHighestSpec_;

    /*
     * Fixed ATR(atrLength) used as the trailing-stop distance.
     *
     * PriceField::Close is a placeholder because the current ATR calculator
     * uses OHLC data directly and ignores IndicatorSpec::source.
     */
    IndicatorSpec atrSpec_;

    /*
     * K in:
     *
     *   highestHighSinceEntry - K * ATR(atrLength)
     */
    double atrMultiplier_ = 0.0;
};
