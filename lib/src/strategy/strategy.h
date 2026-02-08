#pragma once

#include <memory>
#include <vector>
#include <algorithm>

#include "data_types.h"
#include "ranker.h"


/**************************************************************************************
 * Type    : Strategy
 * Purpose : Abstract base class defining generic trading strategy behavior
 *
 * Strategy implements the common execution flow shared by all trading strategies:
 *  - updating existing trades
 *  - ranking the tradable universe
 *  - evaluating entry conditions
 *  - creating new trades
 *
 * Concrete strategies must provide only:
 *  - entry conditions
 *  - trade construction logic
 *  - per-bar trade update logic
 *
 * This design minimizes duplication and makes new strategies easy to implement.
 **************************************************************************************/
class Strategy {
public:
    virtual ~Strategy() = default;

    /**********************************************************************************
     * Purpose : Execute strategy logic for a single timestamp
     *
     * This method is FINAL and defines the invariant execution pipeline:
     *
     *  1. Update all currently open trades (PnL, exits, stop-loss, etc.)
     *  2. Count active (non-simulated) open positions
     *  3. Rank the trading universe using the configured Ranker
     *  4. Attempt new trade entries until limits are reached
     *
     * Concrete strategies customize behavior by overriding:
     *  - shouldEnter()
     *  - buildTrade()
     *  - onBar()
     *
     * Args :
     *   bars                - market data for all coins at the current timestamp
     *   ts                  - current timestamp
     *   last_trade_id       - reference to the global trade identifier counter
     *   current_trades      - reference to currently open trades (modifiable)
     *   strategy_allocation - capital allocated to this strategy
     *
     * Return : None
     **********************************************************************************/
    void calculateSignals(
        const CoinBarMap& bars,
        Timestamp ts,
        unsigned int& last_trade_id,
        std::vector<Trade>& current_trades,
        double strategy_allocation
    );

protected:
    /**************************************************************************************
     * Purpose : Construct a strategy with shared configuration parameters
     *
     * Args :
     *   name                 - human-readable strategy name
     *   maxPosOpen           - maximum number of simultaneous open positions
     *   riskPerTradePctg     - fraction of allocated capital risked per trade
     *   ranker               - ranking algorithm used to order tradable instruments
     *   commissionEntryFactor  - commission factor applied on trade entry
     *   commissionExitFactor - commission factor applied on trade exit
     *   maxRankingPosition   - maximum ranking depth considered for entries
     *
     * Notes :
     *   This constructor is protected because Strategy is an abstract base class
     *   and must not be instantiated directly.
     **************************************************************************************/
    Strategy(std::string name,
             unsigned int maxPosOpen,
             double riskPerTradePctg,
             std::unique_ptr<Ranker> ranker,
             double commissionEntryFactor,
             double commissionExitFactor,
             unsigned int maxRankingPosition)
        : strategy_name_(std::move(name)),
          maxPosOpen_(maxPosOpen),
          riskPerTrade_(riskPerTradePctg),
          ranker_(std::move(ranker)),
          commissionEntryFactor_(commissionEntryFactor),
          commissionExitFactor_(commissionExitFactor),
          maxRankingPosition_(maxRankingPosition)
    {}


    /**************************************************************************************
     * Purpose : Check whether a coin already has an open (non-exited) trade
     *
     * Args :
     *   trades - collection of current open trades
     *   coin   - coin to check
     *
     * Return :
     *   true if a non-exited trade exists for the given coin, false otherwise
     **************************************************************************************/
    bool hasOpenTrade(
        const std::vector<Trade>& trades,
        const Coin& coin
    ) const;


    /**************************************************************************************
     * Purpose : Strategy-specific entry condition
     *
     * Determines whether a new trade should be opened for a given coin.
     **************************************************************************************/
    virtual bool shouldEnter(
        const Coin& coin,
        const BarData& bar,
        const std::vector<Trade>& current_trades
    ) const = 0;

    /**************************************************************************************
     * Purpose : Construct a new trade instance upon entry
     **************************************************************************************/
    virtual Trade buildTrade(
        const Coin& coin,
        const BarData& bar,
        Timestamp ts,
        unsigned int& last_trade_id,
        double strategy_allocation
    ) const = 0;

    /**************************************************************************************
     * Purpose : Update an open trade for the current bar
     *
     * Handles PnL updates, stop-loss logic, exits, and trailing logic.
     **************************************************************************************/
    virtual void onBar(
        Trade& trade,
        const BarData& bar,
        Timestamp ts
    ) const = 0;


protected:
    // ------------------------------------------------------------------
    // Shared strategy configuration
    // ------------------------------------------------------------------

    std::string strategy_name_;           // Human-readable strategy name
    unsigned int maxPosOpen_;             // Maximum concurrent open positions
    double riskPerTrade_;                 // Fraction of capital risked per trade
    std::unique_ptr<Ranker> ranker_;      // Universe ranking algorithm
    unsigned int maxRankingPosition_;     // Max ranking depth for entries
    double commissionEntryFactor_;        // Entry commission factor
    double commissionExitFactor_;         // Exit commission factor
};
