#pragma once

#include <algorithm>
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

class StrategyMRShort final : public Strategy {
public:
    StrategyMRShort(
        unsigned int maxPosOpen,
        double qtyFraction,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int rsiLen,
        double rsiEntry,
        unsigned int btcMALen,
        double entryATRMult,
        unsigned int entryATRLen,
        unsigned int heldBars,
        std::string benchmarkCoin = "BTC",
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "MRShort",
              maxPosOpen,
              qtyFraction,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition,
              std::move(marketFilters)
          ),
          heldBars_(std::max(heldBars, 1u)),
          rsiEntry_(rsiEntry),
          entryATRMult_(entryATRMult),
          benchmarkCoin_(std::move(benchmarkCoin)),
          rsiSpec_{IndicatorKind::RSI, PriceField::Close, rsiLen},
          atrSpec_{IndicatorKind::ATR, PriceField::Close, entryATRLen},
          btcMASpec_{IndicatorKind::SMA, PriceField::Close, btcMALen}
    {}

    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();
        specs.push_back(rsiSpec_);
        specs.push_back(atrSpec_);
        specs.push_back(btcMASpec_);
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
        const BarData* btcBar = findBar(marketData, benchmarkCoin_, ts);

        if (!signalBar || !btcBar) return false;

        const double rsi = indicators.value(coin, ts, rsiSpec_);
        const double atr = indicators.value(coin, ts, atrSpec_);
        const double btcMA = indicators.value(benchmarkCoin_, ts, btcMASpec_);

        if (!std::isfinite(rsi) ||
            !std::isfinite(atr) ||
            !std::isfinite(btcMA) ||
            atr <= 0.0 ||
            btcMA <= 0.0) {
            return false;
        }

        if (coin == "^GSPC") return false;
        if (rsi <= rsiEntry_) return false;
        if (btcBar->close >= btcMA) return false;

        const double entryStop = signalBar->close - entryATRMult_ * atr;
        return std::isfinite(entryStop) && entryStop > 0.0;
    }

    Order buildOrder(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        double strategy_allocation,
        bool,
        const IndicatorEngine& indicators
    ) const override
    {
        Order order;

        order.created_ = ts;
        order.active_from_ = ts;
        order.coin_ = coin;
        order.direction_ = Direction::Short;
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

        const double entryStop = signalBar->close - entryATRMult_ * atr;
        if (!std::isfinite(entryStop) || entryStop <= 0.0) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        const double tradeValue = strategy_allocation * riskPerTrade_;

        order.trigger_price_ = entryStop;
        order.size_ = tradeValue / entryStop;

        // Fixed SL from setup/signal bar high.
        order.sl_ = signalBar->close + atr;
        order.slReference_ = order.sl_;

        return order;
    }

    bool shouldFillOrder(
        const Order& order,
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine&
    ) const override
    {
        if (order.strategy_name_ != strategy_name_) return false;
        if (order.status_ != OrderStatus::Pending) return false;
        if (order.type_ != OrderType::Stop) return false;
        if (order.direction_ != Direction::Short) return false;
        if (ts <= order.created_) return false;
        if (order.trigger_price_ <= 0.0) return false;

        const BarData* bar = findBar(marketData, order.coin_, ts);
        if (!bar) return false;

        return bar->low <= order.trigger_price_;
    }

    Trade buildTradeFromOrder(
        const Order& order,
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        bool,
        const IndicatorEngine& indicators
    ) const override
    {
        if (!shouldFillOrder(order, marketData, ts, indicators)) {
            return invalidTrade();
        }

        const BarData* bar = findBar(marketData, order.coin_, ts);
        if (!bar) return invalidTrade();

        // Short stop entry:
        // if open is already below the stop, fill at open;
        // otherwise fill at stop.
        const double fillPrice = std::min(order.trigger_price_, bar->open);

        if (!std::isfinite(fillPrice) || fillPrice <= 0.0) {
            return invalidTrade();
        }

        const double intendedTradeValue = order.size_ * order.trigger_price_;
        const double finalSize = intendedTradeValue / fillPrice;

        if (!std::isfinite(finalSize) || finalSize <= 0.0) {
            return invalidTrade();
        }

        Trade trade;

        trade.trade_id_ = ++last_trade_id;
        trade.source_order_id_ = order.order_id_;

        trade.start_ = ts;
        trade.end_ = 0;

        trade.coin_ = order.coin_;
        trade.direction_ = Direction::Short;

        trade.entry_ = fillPrice;
        trade.exit_ = 0.0;
        trade.current_price_ = bar->close;

        trade.size_ = finalSize;
        trade.pnl_ = (trade.entry_ - trade.current_price_) * trade.size_;

        trade.commission_ =
            trade.entry_ * trade.size_ * commissionEntryFactor_;

        // Fixed SL from signal/setup bar high.
        trade.sl_ = order.sl_;
        trade.slReference_ = order.slReference_;

        trade.isSimulated_ = false;
        trade.exited_ = false;
        trade.barsHeld = 1;

        trade.strategy_name_ = strategy_name_;

        /*
         * Same-bar short entry + SL RealTest-style intrabar assumption:
         *
         * Bullish entry bar:
         *   open -> low -> high -> close
         *   short entry can happen first, then SL can happen same bar.
         *
         * Bearish entry bar:
         *   open -> high -> low -> close
         *   high happened before short entry, so no same-bar SL.
         */
        const bool bullishEntryBar = bar->close >= bar->open;

        if (bullishEntryBar &&
            trade.sl_ > 0.0 &&
            bar->high >= trade.sl_) {
            closeShortAtPrice(
                trade,
                std::max(trade.sl_, bar->open),
                ts
            );
        }

        return trade;
    }

    bool shouldCancelOrder(
        const Order& order,
        const MarketData&,
        Timestamp ts,
        const IndicatorEngine&
    ) const override
    {
        if (order.strategy_name_ != strategy_name_) return false;
        if (order.status_ != OrderStatus::Pending) return true;
        if (ts <= order.created_) return false;
        return true;
    }

    void onBar(
        Trade& trade,
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        bool,
        const IndicatorEngine&
    ) const override
    {
        if (trade.exited_) return;
        if (ts <= trade.start_) return;

        const BarData* bar = findBar(marketData, coin, ts);
        if (!bar) return;

        trade.current_price_ = bar->close;
        trade.pnl_ = (trade.entry_ - trade.current_price_) * trade.size_;

        // After entry day, fixed SL triggers normally.
        if (trade.sl_ > 0.0 && bar->high >= trade.sl_) {
            closeShortAtPrice(
                trade,
                std::max(trade.sl_, bar->open),
                ts
            );
            return;
        }

        ++trade.barsHeld;

        if (trade.barsHeld < heldBars_) {
            return;
        }

        // Time exit: after HeldBars, exit next open if possible.
        Timestamp nextTs = 0;
        BarData nextBar;

        if (findNextBar(marketData, coin, ts, nextTs, nextBar)) {
            closeShortAtPrice(trade, nextBar.open, nextTs);
        } else {
            closeShortAtPrice(trade, bar->close, ts);
        }
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
        if (tsIt == marketData.end()) return nullptr;

        const auto coinIt = tsIt->second.find(coin);
        if (coinIt == tsIt->second.end()) return nullptr;

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

    void closeShortAtPrice(
        Trade& trade,
        double price,
        Timestamp ts
    ) const
    {
        trade.exit_ = price;
        trade.end_ = ts;
        trade.exited_ = true;
        trade.current_price_ = price;
        trade.pnl_ = (trade.entry_ - trade.exit_) * trade.size_;

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
    unsigned int heldBars_ = 1;
    double rsiEntry_ = 70.0;
    double entryATRMult_ = 0.30;

    std::string benchmarkCoin_;

    IndicatorSpec rsiSpec_;
    IndicatorSpec atrSpec_;
    IndicatorSpec btcMASpec_;
};