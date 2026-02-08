#pragma once

#include <memory>
#include <vector>
#include "data_types.h"
#include "ranker.h"
#include <algorithm>


/**************************************************************************************
 * Type    : Strategy
 * Purpose : Abstract base class defining trading strategy behavior
 *
 * Strategy represents a trading algorithm capable of generating trading signals
 * based on enriched market data. Concrete strategy implementations must override
 * the signal generation logic.
 **************************************************************************************/
class Strategy {
public:
    virtual ~Strategy() = default;

    /**********************************************************************************
     * Purpose : Calculate trading signals for the current timestamp
     *
     * This method is invoked once per timestamp and allows the strategy to:
     *  - open new trades
     *  - update existing trades
     *  - apply entry/exit logic based on market conditions
     *
     * Args    :
     *   bars               - market data for all coins at the current timestamp
     *   ts                 - current timestamp
     *   last_trade_id      - reference to the global trade identifier counter
     *   current_trades     - reference to currently open trades (modifiable)
     *   strategy_allocation- capital allocated to this strategy
     *
     * Return  : None
     **********************************************************************************/
    virtual void calculateSignals(
        const CoinBarMap& bars,
        Timestamp ts,
        unsigned int& last_trade_id,
        std::vector<Trade>& current_trades,
        double strategy_allocation
    ) = 0;

protected:
    /**************************************************************************************
     * Purpose : Construct a strategy with shared configuration parameters
     *
     * Args    : name                - human-readable strategy name
     *           maxPosOpen          - maximum number of simultaneous open positions
     *           riskPerTradePctg    - fraction of allocated capital risked per trade
     *           ranker              - ranking algorithm used to order tradable instruments
     *           commissionEntryPctg - commission percentage applied on trade entry
     *           commissionExitPctg  - commission percentage applied on trade exit
     *
     * Notes   : This constructor is protected as Strategy is an abstract base class
     *           and cannot be instantiated directly.
     **************************************************************************************/
    Strategy(std::string name,
             unsigned int maxPosOpen,
             double riskPerTradePctg,
             std::unique_ptr<Ranker> ranker,
             double commissionEntryPctg,
             double commissionExitPctg)
        : strategy_name_(std::move(name)),
          maxPosOpen_(maxPosOpen),
          riskPerTrade_(riskPerTradePctg),
          ranker_(std::move(ranker)),
          commissionEntryPctg_(commissionEntryPctg),
          commissionExitPctg_(commissionExitPctg)
    {}

    /**************************************************************************************
     * Purpose : Check whether a coin already has an open trade
     *
     * Args    : trades - collection of current open trades
     *           coin   - coin to check
     *
     * Return  : true if a non-exited trade exists for the given coin, false otherwise
     **************************************************************************************/
    bool hasOpenTrade(const std::vector<Trade>& trades, const Coin& coin) const {
        return std::any_of(trades.begin(), trades.end(),
            [&](const Trade& t) {
                return !t.exited_ && t.coin_ == coin;
            });
    }

protected:
    // Strategy metadata
    std::string strategy_name_;

    // Maximum number of concurrent open positions
    unsigned int maxPosOpen_;

    // Fraction of allocated capital risked per trade
    double riskPerTrade_;

    // Ranking algorithm used by the strategy
    std::unique_ptr<Ranker> ranker_;

    // Commission percentages applied on trade entry and exit
    double commissionEntryPctg_;
    double commissionExitPctg_;
};
