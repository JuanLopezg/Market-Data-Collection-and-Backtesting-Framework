#pragma once

#include <memory>
#include <utility>
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
 *
 * A StrategyInstance owns:
 *   - one Strategy
 *   - currently active trades
 *   - pending orders waiting to be filled/cancelled
 *   - the strategy allocation weight
 **************************************************************************************/
class StrategyInstance {
private:
    double& current_global_balance_;
    double& current_global_equity_;

    std::unique_ptr<Strategy> strategy_;

    double weight_ = 0.0;

    std::vector<Trade> current_trades_;
    std::vector<Order> pending_orders_;

public:
    StrategyInstance(
        double& balance,
        double& equity,
        std::unique_ptr<Strategy> strategy,
        double weight,
        std::vector<Trade> current_trades = {},
        std::vector<Order> pending_orders = {}
    )
        : current_global_balance_(balance),
          current_global_equity_(equity),
          strategy_(std::move(strategy)),
          weight_(weight),
          current_trades_(std::move(current_trades)),
          pending_orders_(std::move(pending_orders))
    {}

    /**************************************************************************************
     * Purpose : Invoke strategy logic for the current timestamp
     *
     * The strategy receives:
     *   - active trades
     *   - pending orders
     *   - current allocation
     *
     * This allows strategies to:
     *   - update open trades
     *   - update/cancel pending orders
     *   - create new market/limit/stop orders
     **************************************************************************************/
    void calculateSignals(
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        bool live_trading,
        const IndicatorEngine& indicators
    )
    {
        const double strategy_allocation = current_global_equity_ * weight_;

        strategy_->calculateSignals(
            marketData,
            ts,
            last_trade_id,
            current_trades_,
            pending_orders_,
            strategy_allocation,
            live_trading,
            indicators
        );
    }

    /**************************************************************************************
     * Purpose : Access currently active trades
     **************************************************************************************/
    std::vector<Trade>& GetCurrentTrades()
    {
        return current_trades_;
    }

    const std::vector<Trade>& GetCurrentTrades() const
    {
        return current_trades_;
    }

    /**************************************************************************************
     * Purpose : Access pending orders
     **************************************************************************************/
    std::vector<Order>& GetPendingOrders()
    {
        return pending_orders_;
    }

    const std::vector<Order>& GetPendingOrders() const
    {
        return pending_orders_;
    }

    /**************************************************************************************
     * Purpose : Access strategy weight
     **************************************************************************************/
    double& GetWeight()
    {
        return weight_;
    }

    const double& GetWeight() const
    {
        return weight_;
    }

    /**************************************************************************************
     * Purpose : Access owned strategy
     **************************************************************************************/
    Strategy& GetStrategy()
    {
        return *strategy_;
    }

    const Strategy& GetStrategy() const
    {
        return *strategy_;
    }
};