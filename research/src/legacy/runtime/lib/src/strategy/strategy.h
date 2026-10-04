#pragma once

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "data_types.h"
#include "indicator_spec.h"
#include "market_filter.h"
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
     *
     * Pipeline:
     *   1. Update open trades
     *   2. Update pending orders
     *   3. Check market filters
     *   4. Select universe
     *   5. Rank universe
     *   6. Create either a direct market trade or a pending entry order
     **************************************************************************************/
    void calculateSignals(
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        std::vector<Trade>& current_trades,
        std::vector<Order>& pending_orders,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine& indicators
    );

    /**************************************************************************************
     * Purpose : Return indicators required by this strategy
     *
     * Default implementation returns indicators required by:
     *   - market filters
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
        unsigned int maxRankingPosition,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    );

    /**************************************************************************************
     * Purpose : Check whether a coin already has an open trade
     **************************************************************************************/
    bool hasOpenTrade(
        const std::vector<Trade>& trades,
        const Coin& coin
    ) const;

    /**************************************************************************************
     * Purpose : Check whether a coin already has a pending order
     **************************************************************************************/
    bool hasPendingOrder(
        const std::vector<Order>& orders,
        const Coin& coin
    ) const;

    /**************************************************************************************
     * Purpose : Check whether all strategy-level market filters pass
     *
     * These filters block only new entries.
     * Existing open trades and existing pending orders are updated before this check.
     **************************************************************************************/
    bool marketFiltersPass(
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const;

    /**************************************************************************************
     * Purpose : Tell the base Strategy whether entries should become pending orders
     *
     * Default:
     *   false -> buildTrade() is used directly.
     *
     * Limit-entry / stop-entry strategies should override this and return true.
     **************************************************************************************/
    virtual bool usesEntryOrders() const;

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
     * Purpose : Construct a direct market trade
     *
     * Used when usesEntryOrders() returns false.
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
     * Purpose : Construct a pending entry order
     *
     * Used when usesEntryOrders() returns true.
     *
     * Example:
     *   - limit buy order
     *   - stop buy order
     *   - limit short order
     *   - stop short order
     **************************************************************************************/
    virtual Order buildOrder(
        const Coin& coin,
        const MarketData& marketData,
        Timestamp ts,
        double strategy_allocation,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const;

    /**************************************************************************************
     * Purpose : Decide whether a pending order should be filled on the current bar
     *
     * Limit/stop strategies can override this if they need custom fill logic.
     **************************************************************************************/
    virtual bool shouldFillOrder(
        const Order& order,
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const;

    /**************************************************************************************
     * Purpose : Convert a filled pending order into a real trade
     **************************************************************************************/
    virtual Trade buildTradeFromOrder(
        const Order& order,
        const MarketData& marketData,
        Timestamp ts,
        unsigned int& last_trade_id,
        bool live_trading,
        const IndicatorEngine& indicators
    ) const;

    /**************************************************************************************
     * Purpose : Decide whether a pending order should be cancelled
     *
     * For your case, this is where you can cancel/forget an order if it was not filled
     * on the next bar.
     **************************************************************************************/
    virtual bool shouldCancelOrder(
        const Order& order,
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const;

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

    std::vector<std::unique_ptr<MarketFilter>> marketFilters_;

    unsigned int maxRankingPosition_;

    double commissionEntryFactor_;
    double commissionExitFactor_;
};