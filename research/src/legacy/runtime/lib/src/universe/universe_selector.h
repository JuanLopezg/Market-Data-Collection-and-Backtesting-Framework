#pragma once

#include <vector>

#include "data_types.h"
#include "indicator_spec.h"


class IndicatorEngine;


/**************************************************************************************
 * Type    : UniverseSelector
 * Purpose : Abstract base class for tradable-universe selection
 *
 * A UniverseSelector decides which coins are eligible for ranking and trading at a
 * given timestamp.
 *
 * Example:
 *   - all coins
 *   - top 20 by average volume
 *   - coins above minimum liquidity
 *   - coins with enough history
 *
 * Indicators used by universe selectors must be exposed through requiredIndicators()
 * so BacktestContext can precompute them.
 **************************************************************************************/
class UniverseSelector {
public:
    virtual ~UniverseSelector() = default;

    /**************************************************************************************
     * Purpose : Select eligible coins from the current timestamp universe
     *
     * Args:
     *   bars       - current timestamp market data, coin -> BarData
     *   ts         - current timestamp
     *   indicators - shared indicator engine
     *
     * Return:
     *   CoinBarMap containing only eligible coins
     **************************************************************************************/
    virtual CoinBarMap select(
        const CoinBarMap& bars,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const = 0;

    /**************************************************************************************
     * Purpose : Return indicators required by this universe selector
     **************************************************************************************/
    virtual std::vector<IndicatorSpec> requiredIndicators() const;
};


/**************************************************************************************
 * Type    : AllUniverseSelector
 * Purpose : Default selector that keeps the full universe
 **************************************************************************************/
class AllUniverseSelector : public UniverseSelector {
public:
    CoinBarMap select(
        const CoinBarMap& bars,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const override;
};