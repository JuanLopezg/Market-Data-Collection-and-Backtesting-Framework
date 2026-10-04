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

class StrategyATRBreakout final : public Strategy {
public:
    StrategyATRBreakout(
        unsigned int maxPosOpen,
        double qtyFraction,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int heldBars,
        double atrMultiple,
        unsigned int atrLength,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "ATR_Breakout",
              maxPosOpen,
              qtyFraction,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition,
              std::move(marketFilters)
          ),
          heldBars_(heldBars),
          atrMultiple_(atrMultiple),
          atrSpec_{IndicatorKind::ATR, PriceField::Close, atrLength}
    {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();
        specs.push_back(atrSpec_);
        return specs;
    }

protected:
    bool usesEntryOrders() const override
    {
        return true;
    }

    bool shouldEnter(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        const std::vector<Trade>&,
        double,
        const IndicatorEngine& indicators
    ) const override
    {
        const BarData* signalBar = findBar(marketData, coin, ts);

        if (!signalBar) {
            return false;
        }

        if (signalBar->high <= 0.0) {
            return false;
        }

        const double atr = indicators.value(coin, ts, atrSpec_);

        if (!std::isfinite(atr) || atr <= 0.0) {
            return false;
        }

        const double entryStop =
            signalBar->high + atrMultiple_ * atr;

        return std::isfinite(entryStop) && entryStop > 0.0;
    }

    Order buildOrder(
        const Coin& coin,
        const MarketData& marketData,
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

        const BarData* signalBar = findBar(marketData, coin, ts);

        if (!signalBar) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        const double atr = indicators.value(coin, ts, atrSpec_);

        if (!std::isfinite(atr) || atr <= 0.0) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        const double entryStop =
            signalBar->high + atrMultiple_ * atr;

        if (!std::isfinite(entryStop) || entryStop <= 0.0) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        const double tradeValue = strategy_allocation * riskPerTrade_;

        order.trigger_price_ = entryStop;
        order.size_ = tradeValue / entryStop;

        order.sl_ = 0.0;
        order.slReference_ = entryStop;

        return order;
    }

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

        if (ts <= order.created_) {
            return false;
        }

        if (order.trigger_price_ <= 0.0) {
            return false;
        }

        const BarData* bar = findBar(marketData, order.coin_, ts);

        if (!bar) {
            return false;
        }

        return bar->high >= order.trigger_price_;
    }

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

        if (!shouldFillOrder(order, marketData, ts, indicators)) {
            return invalidTrade();
        }

        const BarData* bar = findBar(marketData, order.coin_, ts);

        if (!bar) {
            return invalidTrade();
        }

        const double fillPrice =
            std::max(order.trigger_price_, bar->open);

        if (!std::isfinite(fillPrice) || fillPrice <= 0.0) {
            return invalidTrade();
        }

        const double intendedTradeValue =
            order.size_ * order.trigger_price_;

        if (!std::isfinite(intendedTradeValue) || intendedTradeValue <= 0.0) {
            return invalidTrade();
        }

        const double finalSize =
            intendedTradeValue / fillPrice;

        if (!std::isfinite(finalSize) || finalSize <= 0.0) {
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
            (trade.current_price_ - trade.entry_) * trade.size_;

        trade.commission_ =
            trade.entry_ * trade.size_ * commissionEntryFactor_;

        trade.sl_ = order.sl_;
        trade.slReference_ = order.slReference_;

        trade.isSimulated_ = false;
        trade.exited_ = false;

        // Entry bar counts as first held bar.
        trade.barsHeld = 1;

        trade.strategy_name_ = strategy_name_;

        return trade;
    }

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

        if (ts <= order.created_) {
            return false;
        }

        return true;
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

        if (trade.exited_) {
            return;
        }

        if (ts <= trade.start_) {
            return;
        }

        const BarData* bar = findBar(marketData, coin, ts);

        if (!bar) {
            return;
        }

        trade.current_price_ = bar->close;

        if (trade.direction_ == Direction::Long) {
            trade.pnl_ = (trade.current_price_ - trade.entry_) * trade.size_;
        } else if (trade.direction_ == Direction::Short) {
            trade.pnl_ = (trade.entry_ - trade.current_price_) * trade.size_;
        }

        ++trade.barsHeld;

        if (trade.barsHeld < heldBars_) {
            return;
        }

        auto nextTsIt = marketData.upper_bound(ts);

        if (nextTsIt != marketData.end()) {
            Timestamp nextTs = nextTsIt->first;

            const auto& nextBars = nextTsIt->second;
            const auto nextBarIt = nextBars.find(coin);

            if (nextBarIt != nextBars.end()) {
                closeTradeAtBarOpen(trade, nextBarIt->second, nextTs);
                return;
            }
        }

        closeTradeAtBarClose(trade, *bar, ts);
    }

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

        if (trade.direction_ == Direction::Long) {
            trade.pnl_ =
                (trade.exit_ - trade.entry_) * trade.size_;
        } else if (trade.direction_ == Direction::Short) {
            trade.pnl_ =
                (trade.entry_ - trade.exit_) * trade.size_;
        } else {
            trade.pnl_ = 0.0;
        }

        trade.commission_ +=
            trade.exit_ * trade.size_ * commissionExitFactor_;
    }

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

        if (trade.direction_ == Direction::Long) {
            trade.pnl_ = (trade.exit_ - trade.entry_) * trade.size_;
        } else if (trade.direction_ == Direction::Short) {
            trade.pnl_ = (trade.entry_ - trade.exit_) * trade.size_;
        } else {
            trade.pnl_ = 0.0;
        }

        trade.commission_ +=
            trade.exit_ * trade.size_ * commissionExitFactor_;
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
    unsigned int heldBars_ = 0;
    double atrMultiple_ = 0.0;

    IndicatorSpec atrSpec_;
};