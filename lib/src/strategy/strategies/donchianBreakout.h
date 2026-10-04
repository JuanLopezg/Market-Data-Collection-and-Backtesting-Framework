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
 * Strategy : StrategyDonchianBreakout
 *
 * Entry:
 *   A long entry signal occurs when the current bar closes above the upper Donchian
 *   Channel calculated from previously completed bars.
 *
 *   Signal:
 *     close > DonchianHigh(lookback)[1]
 *
 *   Optional market-state filter:
 *     benchmark close > SMA(benchmark, benchmarkMovingAverageLength)
 *
 *   The strategy creates a market order after the signal and fills it at the next
 *   available bar open.
 *
 * Exit:
 *   An open trade is closed when:
 *
 *     close < DonchianMid(lookback)
 *
 *   The exit is executed at the current bar close.
 **************************************************************************************/
class StrategyDonchianBreakout final : public Strategy {
public:
    StrategyDonchianBreakout(
        unsigned int maxPosOpen,
        double qtyFraction,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int donchianLookback,
        bool useMarketStateFilter = false,
        unsigned int benchmarkMovingAverageLength = 50,
        std::string benchmarkCoin = "BTC",
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    )
        : Strategy(
              "DonchianBreakout",
              maxPosOpen,
              qtyFraction,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition,
              std::move(marketFilters)
          ),
          useMarketStateFilter_(useMarketStateFilter),
          benchmarkCoin_(std::move(benchmarkCoin)),
          donchianHighSpec_{
              IndicatorKind::DonchianHigh,
              PriceField::High,
              donchianLookback,
              1
          },
          donchianMidSpec_{
              IndicatorKind::DonchianMid,
              PriceField::Close,
              donchianLookback
          },
          benchmarkMASpec_{
              IndicatorKind::SMA,
              PriceField::Close,
              benchmarkMovingAverageLength
          }
    {}


    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();

        specs.push_back(donchianHighSpec_);
        specs.push_back(donchianMidSpec_);

        if (useMarketStateFilter_) {
            specs.push_back(benchmarkMASpec_);
        }

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
        const BarData* bar = findBar(marketData, coin, ts);

        if (!bar) {
            return false;
        }

        if (useMarketStateFilter_) {
            const BarData* benchmarkBar =
                findBar(marketData, benchmarkCoin_, ts);

            if (!benchmarkBar) {
                return false;
            }

            const double benchmarkMA =
                indicators.value(benchmarkCoin_, ts, benchmarkMASpec_);

            if (!std::isfinite(benchmarkMA) ||
                benchmarkMA <= 0.0 ||
                benchmarkBar->close <= benchmarkMA) {
                return false;
            }
        }

        const double donchianHigh =
            indicators.value(coin, ts, donchianHighSpec_);

        return std::isfinite(donchianHigh) &&
               donchianHigh > 0.0 &&
               bar->close > donchianHigh;
    }


    Order buildOrder(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine&
    ) const override
    {
        (void)live_trading;

        Order order;

        order.created_ = ts;
        order.active_from_ = ts;
        order.coin_ = coin;
        order.direction_ = Direction::Long;
        order.type_ = OrderType::Market;
        order.status_ = OrderStatus::Pending;
        order.tif_ = OrderTimeInForce::GoodForBars;
        order.barsValid_ = 1;
        order.barsAlive_ = 0;
        order.strategy_name_ = strategy_name_;

        const BarData* bar = findBar(marketData, coin, ts);

        if (!bar ||
            !std::isfinite(bar->close) ||
            bar->close <= 0.0) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        const double tradeValue =
            strategy_allocation * riskPerTrade_;

        if (!std::isfinite(tradeValue) ||
            tradeValue <= 0.0) {
            order.status_ = OrderStatus::Expired;
            return order;
        }

        order.trigger_price_ = bar->close;
        order.size_ = tradeValue / bar->close;

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

        if (order.type_ != OrderType::Market) {
            return false;
        }

        if (order.direction_ != Direction::Long) {
            return false;
        }

        if (ts <= order.created_) {
            return false;
        }

        const BarData* bar = findBar(marketData, order.coin_, ts);

        return bar &&
               std::isfinite(bar->open) &&
               bar->open > 0.0;
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

        if (!shouldFillOrder(
                order,
                marketData,
                ts,
                indicators
            )) {
            return invalidTrade();
        }

        const BarData* bar = findBar(marketData, order.coin_, ts);

        if (!bar) {
            return invalidTrade();
        }

        const double fillPrice = bar->open;

        if (!std::isfinite(fillPrice) || fillPrice <= 0.0) {
            return invalidTrade();
        }

        const double intendedTradeValue =
            order.size_ * order.trigger_price_;

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
            (trade.current_price_ - trade.entry_) *
            trade.size_;

        trade.commission_ =
            trade.entry_ *
            trade.size_ *
            commissionEntryFactor_;

        trade.sl_ = 0.0;
        trade.slReference_ = 0.0;

        trade.isSimulated_ = false;
        trade.exited_ = false;
        trade.barsHeld = 1;
        trade.strategy_name_ = strategy_name_;

        const double donchianMid =
            indicators.value(order.coin_, ts, donchianMidSpec_);

        const bool exitSignal =
            std::isfinite(donchianMid) &&
            bar->close < donchianMid;

        if (exitSignal) {
            closeTradeAtBarClose(trade, *bar, ts);
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
        const IndicatorEngine& indicators
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
        trade.pnl_ =
            (trade.current_price_ - trade.entry_) *
            trade.size_;

        ++trade.barsHeld;

        const double donchianMid =
            indicators.value(coin, ts, donchianMidSpec_);

        const bool exitSignal =
            std::isfinite(donchianMid) &&
            bar->close < donchianMid;

        if (!exitSignal) {
            return;
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


    Trade invalidTrade() const
    {
        Trade trade;

        trade.strategy_name_ = strategy_name_;
        trade.isSimulated_ = true;
        trade.exited_ = true;

        return trade;
    }


private:
    bool useMarketStateFilter_ = false;
    std::string benchmarkCoin_;

    IndicatorSpec donchianHighSpec_;
    IndicatorSpec donchianMidSpec_;
    IndicatorSpec benchmarkMASpec_;
};
