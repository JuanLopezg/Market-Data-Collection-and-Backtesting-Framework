#include "backtest_context.h"
#include <iostream>

/**************************************************************************************
 * Purpose : Update the backtest execution context for the current timestep
 *
 * This method aggregates realized and unrealized PnL across all strategies,
 * updates balance and equity values, moves closed trades to the trade history,
 * and records a balance/equity snapshot.
 *
 * Args    : None
 * Return  : None
 **************************************************************************************/
void BacktestContext::updateConext(){

    double floatingPNL = 0;
    double balance = this->current_balance_;
    double bef = balance;

    for(auto& strategyInstance : this->strategy_portfolio_){
        auto& current_trades = strategyInstance.GetCurrentTrades();

        for (auto it = current_trades.begin(); it != current_trades.end(); ) {
            Trade& trade = *it;

            if (trade.exited_) { // trades just closed
                if(!trade.isSimulated_){
                    trades_history_[trade.trade_id_] = trade;
                    balance += (trade.pnl_ - trade.commission_);
                }

                it = current_trades.erase(it); 
            } 
            else { // ongoing trades
                if(!trade.isSimulated_){
                    floatingPNL += trade.pnl_ - trade.commission_;
                }
                ++it;
            }
        }

    }

    this->current_balance_ = balance;
    this->current_equity_ = balance + floatingPNL;
    this->balance_equity_historic_.emplace_back(std::make_pair(current_balance_,current_equity_));
}