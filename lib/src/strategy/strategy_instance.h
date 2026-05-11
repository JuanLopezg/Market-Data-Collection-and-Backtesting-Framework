#pragma once

#include <memory>
#include <vector>

#include "strategy.h"


class IndicatorEngine;


/**************************************************************************************
 * Type    : StrategyPortfolio
 * Purpose : Collection of strategy instances participating in the backtest
 **************************************************************************************/
class StrategyInstance;
using StrategyPortfolio = std::vector<StrategyInstance>;


/**************************************************************************************
 * Type    : StrategyInstance
 * Purpose : Encapsulates a strategy with its execution state and allocation
 **************************************************************************************/
class StrategyInstance {
private:
    double& current_global_balance_;
    double& current_global_equity_;

    std::unique_ptr<Strategy> strategy_;

    double weight_ = 0.0;

    std::vector<Trade> current_trades_;

public:
    StrategyInstance(
        double& balance,
        double& equity,
        std::unique_ptr<Strategy> strategy,
        double weight,
        std::vector<Trade> current_trades = {}
    )
        : current_global_balance_(balance),
          current_global_equity_(equity),
          strategy_(std::move(strategy)),
          weight_(weight),
          current_trades_(std::move(current_trades))
    {}

    void calculateSignals(
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        bool live_trading,
        const IndicatorEngine& indicators
    )
    {
        double strategy_allocation = current_global_equity_ * weight_;

        strategy_->calculateSignals(
            marketData,
            ts,
            last_trade_id,
            current_trades_,
            strategy_allocation,
            live_trading,
            indicators
        );
    }

    std::vector<Trade>& GetCurrentTrades()
    {
        return current_trades_;
    }

    const std::vector<Trade>& GetCurrentTrades() const
    {
        return current_trades_;
    }

    double& GetWeight()
    {
        return weight_;
    }

    const double& GetWeight() const
    {
        return weight_;
    }

    Strategy& GetStrategy()
    {
        return *strategy_;
    }

    const Strategy& GetStrategy() const
    {
        return *strategy_;
    }
};