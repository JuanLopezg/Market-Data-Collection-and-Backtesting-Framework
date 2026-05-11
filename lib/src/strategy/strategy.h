#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "data_types.h"
#include "indicator_spec.h"
#include "ranker.h"
#include "universe_selector.h"


class IndicatorEngine;


/**************************************************************************************
 * Type    : Strategy
 * Purpose : Abstract base class defining generic trading strategy behavior
 **************************************************************************************/
class Strategy {
public:
    virtual ~Strategy() = default;

    /**************************************************************************************
     * Purpose : Execute strategy logic for one timestamp
     **************************************************************************************/
    void calculateSignals(
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        std::vector<Trade>& current_trades,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine& indicators
    );

    /**************************************************************************************
     * Purpose : Return indicators required by this strategy
     *
     * Default implementation returns indicators required by:
     *   - universe selector
     *   - ranker
     *
     * Concrete strategies should override this and append their own indicators.
     **************************************************************************************/
    virtual std::vector<IndicatorSpec> requiredIndicators() const;

protected:
    /**************************************************************************************
     * Purpose : Construct a strategy with shared configuration
     **************************************************************************************/
    Strategy(
        std::string name,
        unsigned int maxPosOpen,
        double riskPerTradePctg,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        double commissionEntryFactor,
        double commissionExitFactor,
        unsigned int maxRankingPosition
    );

    /**************************************************************************************
     * Purpose : Check whether a coin already has an open trade
     **************************************************************************************/
    bool hasOpenTrade(
        const std::vector<Trade>& trades,
        const Coin& coin
    ) const;

    /**************************************************************************************
     * Purpose : Strategy-specific entry condition
     **************************************************************************************/
    virtual bool shouldEnter(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        const std::vector<Trade>& current_trades,
        double strategy_allocation,
        const IndicatorEngine& indicators
    ) const = 0;

    /**************************************************************************************
     * Purpose : Construct a new trade
     **************************************************************************************/
    virtual Trade buildTrade(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const = 0;

    /**************************************************************************************
     * Purpose : Update an open trade for the current bar
     **************************************************************************************/
    virtual void onBar(
        Trade& trade,
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const = 0;

protected:
    std::string strategy_name_;

    unsigned int maxPosOpen_;
    double riskPerTrade_;

    std::unique_ptr<UniverseSelector> universeSelector_;
    std::unique_ptr<Ranker> ranker_;

    unsigned int maxRankingPosition_;

    double commissionEntryFactor_;
    double commissionExitFactor_;
};