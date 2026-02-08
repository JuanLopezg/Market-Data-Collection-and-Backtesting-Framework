#pragma once

#include <vector>
#include <filesystem>
#include "strategy.h"
#include <sqlite3.h>
#include "backtest_context.h"


/**************************************************************************************
 * Type    : Backtester
 * Purpose : Drives the execution of a backtest over historical market data
 *
 * The Backtester class is responsible for orchestrating the backtest lifecycle.
 * It iterates over historical market data, triggers signal generation on all
 * strategy instances, updates the global backtest context, and optionally
 * persists results.
 **************************************************************************************/
class Backtester {
public:
    /**************************************************************************************
     * Purpose : Construct a Backtester bound to an existing backtest context
     *
     * Args    : backtest_context - reference to an initialized backtest context
     * Return  : None
     **************************************************************************************/
    explicit Backtester(
        BacktestContext& backtest_context
    );

    /**************************************************************************************
     * Purpose : Execute the main backtest loop
     *
     * Iterates sequentially through the historical market data contained in the
     * backtest context, invokes signal calculation on all strategy instances,
     * updates balances and equity, and tracks execution progress.
     *
     * Args    : None
     * Return  : None
     **************************************************************************************/
    void loop();

    /**************************************************************************************
     * Purpose : Persist all backtest results to disk
     *
     * Creates the SQLite database file and delegates persistence of trades and
     * balance/equity history.
     *
     * Args    : backtest_store_path - full path to the SQLite database file
     * Return  : None
     *
     * Throws  : std::runtime_error on any filesystem or database error
     **************************************************************************************/
    void storeResults(std::filesystem::path& backtest_store_path);

private:
    // Reference to the global backtest execution context
    BacktestContext& backtest_context_;

    // Timestamp corresponding to the first bar of the backtest
    Timestamp starting_date_;

    /**************************************************************************************
     * Purpose : Trigger signal calculation for all strategy instances
     *
     * Forwards the current market snapshot to each strategy instance so that
     * strategies may generate new trades or update existing ones.
     *
     * Args    : bars - market data for all coins at the current timestamp
     *           ts   - current timestamp
     * Return  : None
     **************************************************************************************/
    void calculateSignals(const CoinBarMap& bars, Timestamp ts);

    /**************************************************************************************
     * Purpose : Update the global backtest context
     *
     * Aggregates realized and unrealized PnL across all strategies, updates
     * balance and equity values, and records historical snapshots.
     *
     * Args    : None
     * Return  : None
     **************************************************************************************/
    void updateBacktestContext(){
        this->backtest_context_.updateConext();
    }

    
    /**************************************************************************************
     * Purpose : Persist all trades (open and closed) to the SQLite database
     *
     * Creates the trades table if it does not exist and upserts all known trades.
     * Indexes are created to support efficient post-backtest querying.
     *
     * Args    : db - open SQLite database handle
     * Return  : None
     *
     * Throws  : std::runtime_error on any database error
     **************************************************************************************/
    void storeTrades(sqlite3* db);

    /**************************************************************************************
     * Purpose : Persist balance and equity history to the SQLite database
     *
     * Stores the balance/equity time series accumulated during the backtest.
     * Uses a transaction to ensure atomicity.
     *
     * Args    : db - open SQLite database handle
     * Return  : None
     *
     * Throws  : std::runtime_error on any database error
     **************************************************************************************/
    void storeBalanceEquity(sqlite3* db);
};
