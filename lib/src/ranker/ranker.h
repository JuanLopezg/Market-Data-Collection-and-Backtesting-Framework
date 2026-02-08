#pragma once

#include "data_types.h"
#include <vector>
#include <functional>

/**************************************************************************************
 * Type    : RankedBars
 * Purpose : Container holding references to ranked coin bar data
**************************************************************************************/
using RankedBars =
    std::vector<std::reference_wrapper<
        const std::pair<const Coin, BarData>>>;



        
/**************************************************************************************
 * Type    : Rank
 * Purpose : Base class defining a ranking algorithm for OHLCV bar data
**************************************************************************************/
class Ranker {
public:
    virtual ~Ranker() = default;

    /**************************************************************************************
     * Purpose : Rank a collection of coin bar data using a specific ranking algorithm
     * Args    : bars - map of coins to their associated OHLCV bar data
     * Return  : Vector of references to the ranked bar entries
    **************************************************************************************/
    virtual RankedBars rank(const CoinBarMap& bars) const = 0;
};

/**************************************************************************************
 * Type    : VolumeRank
 * Purpose : Rank coin bar data by descending traded volume
**************************************************************************************/
class VolumeRanker final : public Ranker {
public:
    /**************************************************************************************
     * Purpose : Rank bars based on their traded volume
     * Args    : bars - map of coins to their associated OHLCV bar data
     * Return  : Vector of references to bars ranked by volume (descending)
    **************************************************************************************/
    RankedBars rank(const CoinBarMap& bars) const override;
};
