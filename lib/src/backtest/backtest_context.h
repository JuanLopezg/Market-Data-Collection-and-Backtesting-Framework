#pragma once

#include "data_types.h"
#include <cstdint>
#include <cassert>
#include "strategy_instance.h"


/**************************************************************************************
 * Type    : BacktestContext
 * Purpose : Holds global backtest state and shared execution context
 *
 * This class represents the global state of a backtest execution. It owns and manages
 * the strategy portfolio, tracks balance and equity evolution, aggregates trade history,
 * and provides access to enriched historical market data.
 **************************************************************************************/
class BacktestContext {

private:
    // Reference to enriched historical market data used during the backtest
    const EnrichedData& market_data_;

    // Collection of strategy instances participating in the backtest
    StrategyPortfolio strategy_portfolio_;

    // Current realized account balance
    double current_balance_;

    // Current account equity (balance + unrealized PnL)
    double current_equity_;

    // Monotonically increasing trade identifier
    unsigned int last_trade_id_;

    // Commission percentage for limit orders
    double commission_limit_pctg_;

    // Commission percentage for market orders
    double commission_market_pctg_;

    // Time series of balance and equity snapshots
    std::vector<std::pair<Balance, Equity>> balance_equity_historic_;

    // Historical record of closed trades (including non-simulated trades)
    std::map<TradeID, Trade> trades_history_;

public:
    /**************************************************************************************
     * Purpose : Construct and initialize the backtest execution context
     *
     * Args    : marketData            - enriched historical market data
     *           strategies            - collection of strategy objects (ownership transferred)
     *           initialBalance        - starting account balance
     *           initialEquity         - starting account equity
     *           lastTradeId           - last used trade identifier (for resume support)
     *           commissionLimitPctg   - limit order commission percentage (0.0 - 100.0)
     *           commissionMarketPctg  - market order commission percentage (0.0 - 100.0)
     *
     * Notes   : Each strategy is wrapped into a StrategyInstance and assigned an equal
     *           portfolio weight. Ownership of strategy objects is transferred to the
     *           context.
     **************************************************************************************/
    explicit BacktestContext(const EnrichedData& marketData,
                             std::vector<std::unique_ptr<Strategy>>& strategies,
                             double initialBalance,
                             double initialEquity,
                             unsigned int lastTradeId = 0,
                             double commissionLimitPctg = 0.0,
                             double commissionMarketPctg = 0.0)
        : market_data_(marketData),
          current_balance_(initialBalance),
          current_equity_(initialEquity),
          last_trade_id_(lastTradeId),
          commission_limit_pctg_(commissionLimitPctg),
          commission_market_pctg_(commissionMarketPctg)
    {
        assert(!marketData.empty());
        assert(!strategies.empty());
        assert(initialBalance > 0);
        assert(initialEquity > 0);
        assert(commissionLimitPctg  >= 0.0 && commissionLimitPctg  <= 100.0);
        assert(commissionMarketPctg >= 0.0 && commissionMarketPctg <= 100.0);

        const double weight = 1.0 / strategies.size();

        for (auto& strategy : strategies) {
            strategy_portfolio_.emplace_back(
                StrategyInstance(
                    current_balance_,
                    current_equity_,
                    std::move(strategy),
                    weight
                )
            );
        }
    }

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
    void updateConext();

    /**************************************************************************************
     * Purpose : Access the enriched market data used in the backtest
     * Args    : None
     * Return  : const reference to enriched market data
     **************************************************************************************/
    const EnrichedData& GetMarketData() const {
        return market_data_;
    }

    /**************************************************************************************
     * Purpose : Access the strategy portfolio
     * Args    : None
     * Return  : reference to the strategy portfolio
     **************************************************************************************/
    StrategyPortfolio& GetStrategyPortfolio() {
        return strategy_portfolio_;
    }

    /**************************************************************************************
     * Purpose : Access the current realized balance
     * Args    : None
     * Return  : reference to current balance
     **************************************************************************************/
    double& GetCurrentBalance() {
        return current_balance_;
    }

    /**************************************************************************************
     * Purpose : Access the current account equity
     * Args    : None
     * Return  : reference to current equity
     **************************************************************************************/
    double& GetCurrentEquity() {
        return current_equity_;
    }

    /**************************************************************************************
     * Purpose : Access the last assigned trade identifier
     * Args    : None
     * Return  : reference to the last trade id
     **************************************************************************************/
    unsigned int& GetLastTradeId() {
        return last_trade_id_;
    }

    /**************************************************************************************
     * Purpose : Access the historical balance and equity series
     * Args    : None
     * Return  : reference to balance/equity history
     **************************************************************************************/
    std::vector<std::pair<Balance, Equity>>& GetBalanceEquityHistoric() {
        return balance_equity_historic_;
    }

    /**************************************************************************************
     * Purpose : Access the historical record of closed trades
     * Args    : None
     * Return  : reference to the trade history map
     **************************************************************************************/
    std::map<TradeID, Trade>& GetTradesHistory() {
        return trades_history_;
    }

    /**************************************************************************************
     * Purpose : Access the limit order commission percentage
     * Args    : None
     * Return  : reference to limit commission percentage
     **************************************************************************************/
    double& GetCommissionLimitPctg() {
        return commission_limit_pctg_;
    }

    /**************************************************************************************
     * Purpose : Access the market order commission percentage
     * Args    : None
     * Return  : reference to market commission percentage
     **************************************************************************************/
    double& GetCommissionMarketPctg() {
        return commission_market_pctg_;
    }
};
