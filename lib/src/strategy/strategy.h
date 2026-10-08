#pragma once

#include <memory>
#include <string>
#include <vector>

#include "data_types.h"
#include "indicator_spec.h"
#include "market_filter.h"
#include "ranker.h"
#include "signal_state.h"
#include "universe_selector.h"
#include "trade_record.h"


class IndicatorEngine;


// Abstract base class for strategy/opinion logic
//
// A Strategy reads market data and indicators and updates its SignalState.
// It must not decide position quantity, create orders, process fills or calculate PnL.
class Strategy {
public:
    virtual ~Strategy() = default;

    // Conditional research strategies observe actual campaigns, never invent fills.
    // Default strategies continue to use the existing signal/market-order workflow.
    virtual bool requiresStopEntries() const { return false; }
    virtual bool requiresTradeObservations() const { return requiresStopEntries(); }
    virtual void observeTrades(const std::vector<TradeRecord>&) const {}
    virtual double entryStopPrice(const Coin&) const { return 0.0; }
    virtual double protectiveStopPrice(const Coin&) const { return 0.0; }

    // Update this strategy's current signals for one timestamp
    //
    // Signals are persistent. A strategy only changes a coin when its own rules say the
    // desired exposure has changed; otherwise the existing SignalState remains unchanged.
    virtual void updateSignals(
        const MarketData& marketData,
        Timestamp ts,
        SignalState& signalState,
        const IndicatorEngine& indicators
    ) const = 0;

    // Return indicators required by this strategy
    //
    // Default implementation returns indicators required by:
    // - market filters
    // - universe selector
    // - ranker
    //
    // Concrete strategies should override this and append their own indicators.
    virtual std::vector<IndicatorSpec> requiredIndicators() const;

    const std::string& name() const
    {
        return strategy_name_;
    }

protected:
    // Construct a strategy with shared signal-generation configuration
    Strategy(
        std::string name,
        unsigned int maxActiveSignals,
        std::unique_ptr<UniverseSelector> universeSelector,
        std::unique_ptr<Ranker> ranker,
        unsigned int maxRankingPosition,
        std::vector<std::unique_ptr<MarketFilter>> marketFilters = {}
    );

    // Check whether all strategy-level market filters pass
    // Note    : Market filters should block new signals, not force existing signals to exit
    bool marketFiltersPass(
        const MarketData& marketData,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const;

protected:
    std::string strategy_name_;

    unsigned int maxActiveSignals_;

    std::unique_ptr<UniverseSelector> universeSelector_;
    std::unique_ptr<Ranker> ranker_;

    std::vector<std::unique_ptr<MarketFilter>> marketFilters_;

    unsigned int maxRankingPosition_;
};
