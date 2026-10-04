#pragma once

#include "strategy.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "indicator_engine.h"
#include "indicator_spec.h"
#include "logger.h"
#include "ranker.h"
#include "time_utils.h"
#include "universe_selector.h"


/**************************************************************************************
 * Type    : StrategyBargainChaser
 * Purpose : Mean-reversion / bargain-chasing strategy
 *
 * Entry idea:
 *   - universe selection is handled by UniverseSelector
 *   - ranking is handled by Ranker
 *   - this strategy only checks its own entry rules
 *
 * Typical setup:
 *   UniverseSelector:
 *      Top 20 by SMA(Volume, 25)
 *
 *   Ranker:
 *      ROC(Close, 1), ascending
 *
 *   Strategy condition:
 *      1-day ROC below -fallPercentage
 *      close above SMA(Close, maLength)
 **************************************************************************************/
class StrategyBargainChaser : public Strategy {
private:
    unsigned int nBarsExit_ = 0;
    double fallPercentage_ = 0.0;

    unsigned int maLength_ = 50;

    IndicatorSpec maSpec_;
    IndicatorSpec rocSpec_;

public:
    /**************************************************************************************
     * Purpose : Construct BargainChaser strategy
     **************************************************************************************/
    StrategyBargainChaser(
        unsigned int maxPosOpen,
        double riskPerTrade,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition,
        unsigned int nBarsExit,
        double fallPercentage,
        unsigned int maLength = 50
    )
        : Strategy(
              "BargainChaser",
              maxPosOpen,
              riskPerTrade,
              std::move(universeSelector),
              std::move(ranker),
              commissionEntryFactor,
              commissionExitFactor,
              maxRankingPosition
          ),
          nBarsExit_(nBarsExit),
          fallPercentage_(fallPercentage),
          maLength_(maLength),
          maSpec_{IndicatorKind::SMA, PriceField::Close, maLength},
          rocSpec_{IndicatorKind::ROC, PriceField::Close, 1}
    {}

    /**************************************************************************************
     * Purpose : Return indicators required directly by this strategy
     *
     * Base Strategy already includes:
     *   - universe selector indicators
     *   - ranker indicators
     *
     * This strategy adds:
     *   - SMA(Close, maLength)
     *   - ROC(Close, 1)
     **************************************************************************************/
    std::vector<IndicatorSpec> requiredIndicators() const override
    {
        std::vector<IndicatorSpec> specs = Strategy::requiredIndicators();

        specs.push_back(maSpec_);
        specs.push_back(rocSpec_);

        return specs;
    }

protected:
    /**************************************************************************************
     * Purpose : Determine whether a new trade should be opened
     **************************************************************************************/
    bool shouldEnter(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        const std::vector<Trade>&,
        double,
        const IndicatorEngine& indicators
    ) const override
    {
        const BarData& bar = marketData.at(ts).at(coin);

        const double ma = indicators.value(coin, ts, maSpec_);
        const double roc = indicators.value(coin, ts, rocSpec_);
        const double maxAllowedRoc = -fallPercentage_ / 100.0;

        return
            std::isfinite(ma) && // ma has enough data to be calculated : not nan
            std::isfinite(roc) &&  // roc has enough data to be calculated : not nan
            ma > 0.0 &&
            roc < maxAllowedRoc &&
            bar.close > ma;
        }


    /**************************************************************************************
     * Purpose : Construct a new trade
     **************************************************************************************/
    Trade buildTrade(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        double allocation,
        bool live_trading,
        const IndicatorEngine&
    ) const override
    {
        const unsigned int nd = nextDay(ts);

        const bool canFindNextDay =
            marketData.find(nd) != marketData.end() &&
            marketData.at(nd).find(coin) != marketData.at(nd).end();

        Trade trade;

        trade.trade_id_ = ++last_trade_id;
        trade.start_ = nd;
        trade.coin_ = coin;
        trade.direction_ = Direction::Long;
        trade.strategy_name_ = strategy_name_;

        if (canFindNextDay && !live_trading) {
            trade.current_price_ = marketData.at(nd).at(coin).open;
            trade.entry_ = marketData.at(nd).at(coin).open;
        } else {
            trade.current_price_ = marketData.at(ts).at(coin).close;
            trade.entry_ = marketData.at(ts).at(coin).close;
        }

        if (!canFindNextDay && !live_trading) {
            LG_ERROR(
                "Could not find next day for coin {} and date {} when building the trade, using close instead of next open",
                coin,
                ts
            );
        }

        const double rawSize =
            riskPerTrade_ * allocation / marketData.at(ts).at(coin).close;

        trade.size_ =
            std::round(rawSize * 1'000'000.0) / 1'000'000.0;

        trade.isSimulated_ = false;

        trade.commission_ +=
            trade.size_ * trade.entry_ * commissionEntryFactor_;

        return trade;
    }


    /**************************************************************************************
     * Purpose : Update open trade
     **************************************************************************************/
    void onBar(
        Trade& trade,
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        bool live_trading,
        const IndicatorEngine&
    ) const override
    {
        const unsigned int nd = nextDay(ts);

        const bool canFindNextDay =
            marketData.find(nd) != marketData.end() &&
            marketData.at(nd).find(coin) != marketData.at(nd).end();

        if (canFindNextDay && !live_trading) {
            trade.current_price_ = marketData.at(nd).at(coin).open;
        } else {
            trade.current_price_ = marketData.at(ts).at(coin).close;
        }

        if (!canFindNextDay && !live_trading) {
            LG_ERROR(
                "Could not find next day for coin {} and date {} when updating/closing trade, using close instead of next open",
                coin,
                ts
            );
        }

        trade.end_ = nd;
        trade.barsHeld++;

        const double rawPnl =
            trade.size_ *
            (trade.current_price_ - trade.entry_) *
            static_cast<int>(trade.direction_);

        trade.pnl_ =
            std::round(rawPnl * 100.0) / 100.0;

        if (trade.barsHeld == nBarsExit_) {
            trade.exit_ = trade.current_price_;
            trade.exited_ = true;

            trade.commission_ +=
                trade.size_ * trade.exit_ * commissionExitFactor_;

            return;
        } 
    }
};