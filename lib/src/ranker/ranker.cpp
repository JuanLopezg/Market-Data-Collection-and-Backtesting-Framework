
#include "ranker.h"
#include <algorithm>


/**************************************************************************************
 * Purpose : Rank bars based on their traded volume
 * Args    : bars - map of coins to their associated OHLCV bar data
 * Return  : Vector of references to bars ranked by volume (descending)
**************************************************************************************/
RankedBars VolumeRanker::rank(const CoinBarMap& bars) const {
    RankedBars ranked;
    ranked.reserve(bars.size());

    for (const auto& kv : bars) {
        ranked.emplace_back(kv);
    }

    std::sort(ranked.begin(), ranked.end(),
        [](const auto& a, const auto& b) {
            return a.get().second.volume > b.get().second.volume;
        });

    return ranked;
}