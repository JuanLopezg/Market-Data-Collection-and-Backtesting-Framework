#include "backtest_context.h"


/**************************************************************************************
 * Purpose : Update the backtest execution context for the current timestep
 *
 * Aggregates realized and unrealized PnL across all strategies, updates balance/equity,
 * moves closed trades to trade history, and records a balance/equity snapshot.
 **************************************************************************************/
void BacktestContext::updateConext()
{
    double floatingPNL = 0.0;
    double balance = current_balance_;

    for (auto& strategyInstance : strategy_portfolio_) {
        auto& currentTrades = strategyInstance.GetCurrentTrades();

        for (auto it = currentTrades.begin(); it != currentTrades.end(); ) {
            Trade& trade = *it;

            if (trade.exited_) {
                if (!trade.isSimulated_) {
                    trades_history_[trade.trade_id_] = trade;
                    balance += trade.pnl_ - trade.commission_;
                }

                it = currentTrades.erase(it);
            } else {
                if (!trade.isSimulated_) {
                    floatingPNL += trade.pnl_ - trade.commission_;
                }

                ++it;
            }
        }
    }

    current_balance_ = balance;
    current_equity_ = balance + floatingPNL;

    balance_equity_historic_.emplace_back(
        std::make_pair(current_balance_, current_equity_)
    );
}