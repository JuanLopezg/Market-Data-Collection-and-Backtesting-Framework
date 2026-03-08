#pragma once

#include "strategy.h"
#include <memory>


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
 * StrategyInstance binds a concrete Strategy implementation to its runtime context.
 * It owns the strategy object, tracks its open trades, and manages capital allocation
 * relative to the global backtest balance and equity.
 **************************************************************************************/
class StrategyInstance {

private:
    // Reference to the global realized balance
    double& current_global_balance_;

    // Reference to the global equity (balance + unrealized PnL)
    double& current_global_equity_;

    // Owned strategy implementation
    std::unique_ptr<Strategy> strategy_;

    // Fraction of global equity allocated to this strategy
    double weight_ = 0.0;

    // Currently open trades managed by this strategy
    std::vector<Trade> current_trades_;

public:
    /**************************************************************************************
     * Purpose : Construct a strategy instance with execution context
     *
     * Args    : balance        - reference to the global realized balance
     *           equity         - reference to the global equity
     *           strategy       - strategy object (ownership transferred)
     *           weight         - fraction of equity allocated to this strategy
     *           current_trades - initial set of open trades (optional)
     *
     * Notes   : The strategy instance does not own the balance or equity values;
     *           it only maintains references to the global backtest state.
     **************************************************************************************/
    StrategyInstance(double& balance,
                     double& equity,
                     std::unique_ptr<Strategy> strategy,
                     double weight,
                     std::vector<Trade> current_trades = {})
        : current_global_balance_(balance),
          current_global_equity_(equity),
          strategy_(std::move(strategy)),
          weight_(weight),
          current_trades_(std::move(current_trades))
    {}

    /**************************************************************************************
     * Purpose : Invoke strategy signal generation for the current timestamp
     *
     * Computes the capital allocation for the strategy based on global equity
     * and forwards the current market snapshot to the strategy implementation.
     *
     * Args    : bars                - market data for all coins at the current timestamp
     *           ts                  - current timestamp
     *           last_trade_id       - reference to the global trade identifier counter
     *           strategy_allocation - total strategy monetary allocation
     * Return  : None
     **************************************************************************************/
    void calculateSignals(const CoinBarMap& bars,
                          Timestamp ts,
                          unsigned int& last_trade_id)
    {
        double strategy_allocation = current_global_equity_ * weight_;
        strategy_->calculateSignals(
            bars,
            ts,
            last_trade_id,
            current_trades_,
            strategy_allocation
        );
    }

    /**************************************************************************************
     * Purpose : Access the currently open trades for this strategy
     * Args    : None
     * Return  : reference to the vector of open trades
     **************************************************************************************/
    std::vector<Trade>& GetCurrentTrades() {
        return current_trades_;
    }

    /**************************************************************************************
     * Purpose : Access the strategy allocation weight
     * Args    : None
     * Return  : reference to the strategy weight
     **************************************************************************************/
    double& GetWeight() {
        return weight_;
    }
};
