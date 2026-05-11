#pragma once

#include <vector>

#include "data_types.h"
#include "indicator_spec.h"


class IndicatorEngine;


/**************************************************************************************
 * Type    : RankedCoin
 * Purpose : Represents one ranked instrument in the tradable universe
 *
 * Members :
 *   coin  - coin symbol
 *   bar   - pointer to the current bar data for this coin
 *   score - ranking score used to sort the universe
 *   rank  - final 1-based rank after sorting
 **************************************************************************************/
struct RankedCoin {
    Coin coin;
    const BarData* bar = nullptr;
    double score = 0.0;
    unsigned int rank = 0;
};


/**************************************************************************************
 * Type    : RankedUniverse
 * Purpose : Ordered list of ranked tradable instruments
 **************************************************************************************/
using RankedUniverse = std::vector<RankedCoin>;


/**************************************************************************************
 * Type    : Ranker
 * Purpose : Abstract base class for all universe ranking methods
 *
 * A Ranker receives the current market snapshot and returns the tradable universe
 * ordered according to a score.
 *
 * Concrete rankers may depend on indicators. If so, they must expose those required
 * indicators through requiredIndicators(), so the IndicatorEngine can precompute them.
 **************************************************************************************/
class Ranker {
public:
    virtual ~Ranker() = default;

    /**************************************************************************************
     * Purpose : Rank the current universe
     *
     * Args :
     *   bars       - current timestamp market data, coin -> BarData
     *   ts         - current timestamp
     *   indicators - shared indicator engine
     *
     * Return :
     *   RankedUniverse sorted by the concrete ranker's logic
     **************************************************************************************/
    virtual RankedUniverse rank(
        const CoinBarMap& bars,
        Timestamp ts,
        const IndicatorEngine& indicators
    ) const = 0;

    /**************************************************************************************
     * Purpose : Return all indicators required by this ranker
     *
     * Return :
     *   Empty by default. Indicator-based rankers override this.
     **************************************************************************************/
    virtual std::vector<IndicatorSpec> requiredIndicators() const;
};